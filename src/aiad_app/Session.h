// Application state shared by the UI and the Command Bus handlers: the orbit camera and the session
// (drawing -> machine study -> simulation -> scene).
#pragma once

#include "Document.h"
#include "DocumentScene.h"
#include "MechanismStudy.h"

#include "machine/Machine.h"
#include "magneto/MagnetoField.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/gf/rotation.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <numbers>
#include <string>
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

// ---- Session ----------------------------------------------------------------------------------------------

// A time history of the running machine (plots, the agent).
struct Channel
{
    std::string name, unit;
    std::deque<double> values;
};

struct Session
{
    std::unique_ptr<doc::Document> document;    // the drawing (single source of truth)
    // The machine of the drawing: bodies, joints and elements, every input measured on the parts; a
    // document without joints is not a mechanism yet.
    MechanismStudy study;
    bool isMechanism = false;
    std::string studyError;                     // why the document cannot be simulated (empty = it can)
    // Field table of each magneto of the study (MFEM, from its drawn geometry; cached on disk).
    std::vector<std::vector<sim::magneto::MagnetoSample>> magnetoTables;
    std::vector<sim::magneto::MagnetoGeometry> tableGeometry;
    std::unique_ptr<sim::machine::Machine> machine;
    std::unique_ptr<DocumentScene> scene;
    bool running = false;
    double timeScale = 0.02;           // slow motion: 1/50 of real time
    double sparkUntil = 0.0;           // wall-clock time until which the spark is drawn
    std::deque<double> historyTime;
    std::vector<Channel> channels;
    double nextSample = 0.0;
    static constexpr double kSamplePeriod = 1.0e-4;
    static constexpr size_t kHistory = 8000;

    // After any change of the document: derive the study again; the simulation restarts with it.
    bool Build()
    {
        studyError.clear();
        machine.reset();
        isMechanism = document && DeriveMechanismStudy(*document, study);
        if (!isMechanism)
        {
            study = MechanismStudy();
            studyError = "The document has no joints: it is a drawing, not a mechanism yet. Fix its base (aiad_body_define fixed: true), join "
                         "the bodies (aiad_joint_define) and add what else it is made of (aiad_element_define).";
            running = false;
            return false;
        }
        if (!study.Runnable())
        {
            studyError = std::format("{} problem(s) in the drawing:\n{}", study.Errors(), study.Report());
            running = false;
            return false;
        }
        Restart();
        return true;
    }
    // Field tables stay valid while the drawn magneto geometry does not change.
    bool TablesReady() const
    {
        bool same = tableGeometry.size() == study.machine.magnetos.size() && magnetoTables.size() == tableGeometry.size();
        for (size_t i = 0; same && i < tableGeometry.size(); ++i)
            same = std::memcmp(&tableGeometry[i], &study.machine.magnetos[i].geometry, sizeof(sim::magneto::MagnetoGeometry)) == 0;
        return same;
    }
    bool NeedsTables() const { return isMechanism && studyError.empty() && !TablesReady(); }
    // The field tables of the study's magnetos (slow the first time: run off the main thread).
    static std::vector<std::vector<sim::magneto::MagnetoSample>> ComputeTables(const std::vector<sim::magneto::MagnetoGeometry>& geometry,
                                                                               const std::string& cacheFolder)
    {
        std::vector<std::vector<sim::magneto::MagnetoSample>> tables;
        for (size_t i = 0; i < geometry.size(); ++i)
        {
            const std::string cache = std::format("{}/magneto_table{}.txt", cacheFolder, i == 0 ? std::string() : std::to_string(i));
            tables.push_back(sim::magneto::CachedMagnetoTable(cache.c_str(), geometry[i], {}, 45.0, 1.0));
        }
        return tables;
    }
    std::vector<sim::magneto::MagnetoGeometry> MagnetoGeometry() const
    {
        std::vector<sim::magneto::MagnetoGeometry> list;
        for (const sim::machine::Magneto& m : study.machine.magnetos)
            list.push_back(m.geometry);
        return list;
    }
    void SetTables(std::vector<std::vector<sim::magneto::MagnetoSample>> tables, std::vector<sim::magneto::MagnetoGeometry> geometry)
    {
        magnetoTables = std::move(tables);
        tableGeometry = std::move(geometry);
    }

    void Restart()
    {
        machine.reset();
        historyTime.clear();
        channels.clear();
        nextSample = 0.0;
        if (!isMechanism || !studyError.empty() || !TablesReady())
            return;
        machine = std::make_unique<sim::machine::Machine>(study.machine, magnetoTables);
        const sim::mb::Model& m = study.machine.mechanism;
        for (const sim::mb::Joint& j : m.joints)
            if (j.type == sim::mb::JointType::Revolute || j.type == sim::mb::JointType::Cylindrical)
                channels.push_back({j.name + " speed", "rpm"});
            else if (j.type == sim::mb::JointType::Prismatic)
                channels.push_back({j.name + " position", "mm"});
        for (const sim::machine::Chamber& c : study.machine.chambers)
        {
            channels.push_back({c.name + " pressure", "bar"});
            channels.push_back({c.name + " volume", "cm^3"});
        }
        for (const sim::machine::Magneto& g : study.machine.magnetos)
            channels.push_back({g.name + " secondary", "kV"});
        for (const sim::mb::Contact& c : m.contacts)
            channels.push_back({c.name + " force", "N"});
        for (const sim::mb::Spring& s : m.springs)
            channels.push_back({s.name + " force", "N"});
    }

    double Time() const { return machine ? machine->Time() : 0.0; }

    // Every moving body: p = rest origin + M p_body. Rigid bodies follow the solver (about their centre
    // of mass); spring bodies are stretched between the spring's ends.
    std::vector<BodyMotion> Motion() const
    {
        std::vector<BodyMotion> motion;
        if (!machine)
            return motion;
        const sim::mb::Multibody& mb = machine->Mechanism();
        const sim::mb::Model& model = study.machine.mechanism;
        auto affine = [](const std::array<GfVec3d, 3>& columns, const GfVec3d& t) {
            pxr::GfMatrix4d m(1.0);
            for (int k = 0; k < 3; ++k)
                m.SetRow(k, pxr::GfVec4d(columns[size_t(k)][0], columns[size_t(k)][1], columns[size_t(k)][2], 0.0));
            m.SetRow(3, pxr::GfVec4d(t[0], t[1], t[2], 1.0));
            return m;
        };
        for (size_t i = 0; i < model.bodies.size(); ++i)
        {
            const sim::mb::Body& b = model.bodies[i];
            if (b.fixed)
                continue;
            const sim::mb::BodyState s = mb.State(int(i));
            const kernel::Vec3 o = study.origins.at(study.bodies[i]);
            const pxr::GfRotation rotation(pxr::GfQuatd(s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3]));
            const GfVec3d origin(o.x, o.y, o.z), rest(b.centroid[0], b.centroid[1], b.centroid[2]), now(s.centroid[0], s.centroid[1], s.centroid[2]);
            motion.push_back({study.bodies[i], affine({rotation.TransformDir(GfVec3d(1, 0, 0)), rotation.TransformDir(GfVec3d(0, 1, 0)),
                                                       rotation.TransformDir(GfVec3d(0, 0, 1))},
                                                      now - origin - rotation.TransformDir(rest - origin))});
        }
        for (const MechanismStudy::SpringBody& sb : study.springBodies)
        {
            const sim::mb::Spring& s = model.springs[size_t(sb.spring)];
            const GfVec3d r0(s.point0[0], s.point0[1], s.point0[2]), r1(s.point1[0], s.point1[1], s.point1[2]);
            const sim::mb::Vec3 q0 = mb.PointOf(s.body0, s.point0), q1 = mb.PointOf(s.body1, s.point1);
            const GfVec3d p0(q0[0], q0[1], q0[2]), p1(q1[0], q1[1], q1[2]);
            const double restLength = (r1 - r0).GetLength();
            if (restLength < 1e-9)
                continue;
            const GfVec3d u = (r1 - r0) / restLength;
            const double stretch = (p1 - p0).GetLength() / restLength;
            const pxr::GfRotation rotation(r1 - r0, p1 - p0);
            std::array<GfVec3d, 3> columns;
            for (int k = 0; k < 3; ++k)
            {
                GfVec3d e(0.0);
                e[k] = 1.0;
                columns[size_t(k)] = rotation.TransformDir(e + u * ((stretch - 1.0) * pxr::GfDot(e, u)));   // stretch along the spring, then turn
            }
            const kernel::Vec3 o = study.origins.at(sb.body);
            const GfVec3d origin(o.x, o.y, o.z);
            // T(p) = L p + t with p_model = origin + p: L (origin - r0) + p0 - origin.
            GfVec3d shifted = origin - r0;
            const GfVec3d mapped = columns[0] * shifted[0] + columns[1] * shifted[1] + columns[2] * shifted[2];
            motion.push_back({sb.body, affine(columns, mapped + p0 - origin)});
        }
        return motion;
    }

    void Sample()
    {
        const sim::mb::Multibody& mb = machine->Mechanism();
        const sim::mb::Model& m = study.machine.mechanism;
        std::vector<double> values;
        for (size_t j = 0; j < m.joints.size(); ++j)
            if (m.joints[j].type == sim::mb::JointType::Revolute || m.joints[j].type == sim::mb::JointType::Cylindrical)
                values.push_back(mb.Rate(int(j)) * 30.0 / kPi);
            else if (m.joints[j].type == sim::mb::JointType::Prismatic)
                values.push_back(mb.Coordinate(int(j)) * 1e3);
        for (size_t c = 0; c < study.machine.chambers.size(); ++c)
        {
            values.push_back(machine->Chamber(c).pressure / 1e5);
            values.push_back(machine->Chamber(c).volume * 1e6);
        }
        for (size_t g = 0; g < study.machine.magnetos.size(); ++g)
            values.push_back(machine->Magneto(g).lastSecondaryKv);
        for (size_t c = 0; c < m.contacts.size(); ++c)
            values.push_back(mb.ContactForce(int(c)));
        for (size_t s = 0; s < m.springs.size(); ++s)
            values.push_back(mb.SpringForce(int(s)));
        historyTime.push_back(machine->Time());
        for (size_t k = 0; k < channels.size() && k < values.size(); ++k)
            channels[k].values.push_back(values[k]);
        if (historyTime.size() > kHistory)
        {
            historyTime.pop_front();
            for (Channel& c : channels)
                c.values.pop_front();
        }
    }

    void Advance(double wallDt, double wallTime)
    {
        if (!running || !machine)
            return;
        const double dt = machine->StepSize();
        const int steps = std::clamp(int(wallDt * timeScale / dt), 1, 4000);
        for (int i = 0; i < steps; ++i)
        {
            machine->Step(dt);
            for (size_t g = 0; g < study.machine.magnetos.size(); ++g)
                if (machine->Magneto(g).spark)
                    sparkUntil = wallTime + 0.15;
            if (machine->Time() >= nextSample)
            {
                Sample();
                nextSample = machine->Time() + std::max(kSamplePeriod, dt);
            }
        }
    }
};

}  // namespace aiad::app
