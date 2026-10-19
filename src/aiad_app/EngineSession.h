// Application state shared by the UI and the Command Bus handlers: the orbit camera and the engine
// session (design -> model -> simulation -> scene).
#pragma once

#include "Document.h"
#include "DocumentScene.h"
#include "EngineStudy.h"

#include "engine/EngineSimulation.h"
#include "magneto/MagnetoField.h"
#include "magneto/MagnetoIgnition.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <numbers>
#include <vector>

namespace aiad::app
{

using pxr::GfVec3d;
inline constexpr double kPi = std::numbers::pi;

// ---- Camera ---------------------------------------------------------------------------------------

struct OrbitCamera
{
    double yaw = -78.0, pitch = 12.0, distance = 0.62, fovDeg = 35.0;
    GfVec3d target{0.0, 0.0, 0.13};

    pxr::GfMatrix4d CameraToWorld() const
    {
        const double y = yaw * kPi / 180.0, p = pitch * kPi / 180.0;
        const GfVec3d eye = target + distance * GfVec3d(std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p));
        pxr::GfMatrix4d view;
        view.SetLookAt(eye, target, GfVec3d(0, 0, 1));
        return view.GetInverse();
    }
    // Camera axes in world space (camera looks down -back).
    void Axes(GfVec3d& right, GfVec3d& up, GfVec3d& back) const
    {
        const pxr::GfMatrix4d m = CameraToWorld();
        right = GfVec3d(m[0][0], m[0][1], m[0][2]);
        up = GfVec3d(m[1][0], m[1][1], m[1][2]);
        back = GfVec3d(m[2][0], m[2][1], m[2][2]);
    }
    // World ray through a point of the image, (u, v) in [0, 1] from the top-left corner.
    void Ray(double u, double v, double aspect, GfVec3d& origin, GfVec3d& direction) const
    {
        GfVec3d right, up, back;
        Axes(right, up, back);
        const double t = std::tan(fovDeg * kPi / 360.0);
        origin = CameraToWorld().ExtractTranslation();
        direction = (-back + right * ((2.0 * u - 1.0) * t * aspect) + up * ((1.0 - 2.0 * v) * t)).GetNormalized();
    }
    void Pan(double dx, double dy, double viewportHeight)
    {
        GfVec3d right, up, back;
        Axes(right, up, back);
        const double metresPerPixel = 2.0 * distance * std::tan(fovDeg * kPi / 360.0) / viewportHeight;
        target += (-right * dx + up * dy) * metresPerPixel;
    }
    void Fit(const pxr::GfRange3d& box)
    {
        if (box.IsEmpty())
            return;
        target = box.GetMidpoint();
        const double radius = box.GetSize().GetLength() * 0.5;
        distance = radius / std::sin(fovDeg * kPi / 360.0) * 1.05;
    }
};

// ---- Engine session ---------------------------------------------------------------------------------

struct EngineSession
{
    std::unique_ptr<doc::Document> document;    // the drawing (single source of truth)
    EngineStudy study;                          // simulation inputs derived from the drawing
    std::string studyError;                     // why the document cannot be simulated (empty = ok)
    std::vector<sim::magneto::MagnetoSample> magnetoTable;
    std::unique_ptr<sim::engine::EngineSimulation> simulation;
    std::unique_ptr<DocumentScene> scene;
    sim::engine::EngineSample last;
    bool running = false;
    double timeScale = 0.02;           // slow motion: 1/50 of real time
    double sparkUntil = 0.0;           // wall-clock time until which the spark is drawn
    std::vector<double> pressureByAngle = std::vector<double>(720, 1.0);
    std::vector<double> pvVolume, pvPressure, pvShownVolume, pvShownPressure;
    std::deque<double> historyTime, historyRpm, historyTorque;
    int lastCycle = -1;
    int combustions = 0;
    double lastSecondaryKv = 0.0;      // magneto secondary peak at the last points opening

    // After any change of the document: derive the study again and restart the simulation.
    bool Build()
    {
        studyError.clear();
        if (!document || !DeriveEngineStudy(*document, study, studyError))
        {
            simulation.reset();
            running = false;
            return false;
        }
        Restart();
        return true;
    }

    void Restart()
    {
        if (!studyError.empty() || magnetoTable.empty())
            return;
        simulation = std::make_unique<sim::engine::EngineSimulation>(study.simulation, magnetoTable);
        last = {};
        std::fill(pressureByAngle.begin(), pressureByAngle.end(), 1.0);
        pvVolume.clear();
        pvPressure.clear();
        pvShownVolume.clear();
        pvShownPressure.clear();
        historyTime.clear();
        historyRpm.clear();
        historyTorque.clear();
        lastCycle = -1;
        combustions = 0;
        lastSecondaryKv = 0.0;
    }

    // Body motion for the view (mechanism frame, relative to each body's rest placement).
    std::vector<BodyMotion> Motion() const
    {
        if (!simulation)
            return {};
        const auto pose = simulation->CurrentPose();
        const double crankDeg = pose.crankAngle * 180.0 / kPi, r = study.layout.crankRadius, L = study.layout.springLength;
        return {{"Crankshaft", crankDeg},
                {"Rod", pose.rodAngle * 180.0 / kPi, GfVec3d(r * std::cos(pose.crankAngle), r * std::sin(pose.crankAngle), 0.0)},
                {"Piston", 0.0, GfVec3d(pose.pistonX, 0.0, 0.0)},
                {"Camshaft", crankDeg / 2.0},
                {"IntakeValve", 0.0, GfVec3d(-last.intakeLift, 0.0, 0.0)},
                {"ExhaustValve", 0.0, GfVec3d(-last.exhaustLift, 0.0, 0.0)},
                {"IntakeSpring", 0.0, GfVec3d(0.0), L > 0 ? (L - last.intakeLift) / L : 1.0},
                {"ExhaustSpring", 0.0, GfVec3d(0.0), L > 0 ? (L - last.exhaustLift) / L : 1.0}};
    }

    void Advance(double wallDt, double wallTime)
    {
        if (!running || !simulation)
            return;
        const double dt = 2.0e-5;
        const int steps = std::clamp(int(wallDt * timeScale / dt), 1, 2000);
        double torque = 0.0;
        for (int i = 0; i < steps; ++i)
        {
            const bool burning = last.combustion;
            last = simulation->Step(dt);
            torque += last.gasTorque;
            if (last.secondaryPeak > 0.0)
                lastSecondaryKv = last.secondaryPeak / 1e3;
            const double cycleDeg = std::fmod(std::fmod(last.crankDeg, 720.0) + 720.0, 720.0);
            pressureByAngle[std::min(int(cycleDeg), 719)] = last.pressure / 1e5;
            const int cycle = int(std::floor(last.crankDeg / 720.0));
            if (cycle != lastCycle)
            {
                pvShownVolume.swap(pvVolume);
                pvShownPressure.swap(pvPressure);
                pvVolume.clear();
                pvPressure.clear();
                lastCycle = cycle;
            }
            if (i % 2 == 0)
            {
                pvVolume.push_back(last.volume * 1e6);
                pvPressure.push_back(last.pressure / 1e5);
            }
            if (last.combustion && !burning)
            {
                ++combustions;
                sparkUntil = wallTime + 0.15;
            }
        }
        historyTime.push_back(last.time);
        historyRpm.push_back(last.rpm);
        historyTorque.push_back(torque / steps);
        while (historyTime.size() > 2000)
        {
            historyTime.pop_front();
            historyRpm.pop_front();
            historyTorque.pop_front();
        }
    }
};

}  // namespace aiad::app