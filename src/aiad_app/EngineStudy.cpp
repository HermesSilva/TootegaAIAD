#include "EngineStudy.h"

#include "Features.h"

#include <algorithm>
#include <cmath>

namespace aiad::app
{

namespace
{

struct Reader
{
    const doc::Document& d;
    std::string& error;
    bool ok = true;

    double operator()(const char* name)
    {
        double v = 0;
        std::string e;
        if (!d.Value(name, v, e))
        {
            if (ok)
                error = std::string("engine study: ") + e;
            ok = false;
        }
        return v;
    }
};

// Lift per cycle degree measured on the lobe as drawn (the extruded sketch of the part with `role`).
bool MeasureLobe(const doc::Document& d, const char* role, std::vector<double>& lift, double& baseRadius, std::string& error)
{
    for (const doc::PartInfo& p : d.Parts())
    {
        if (p.role != role)
            continue;
        for (const Json& f : p.features)
            if (f.value("op", "") == "extrude" && f.contains("profile"))
            {
                std::vector<std::array<double, 2>> outline;
                if (!doc::SketchOutline(f["profile"], d.Lookup(), outline, error))
                    return false;
                baseRadius = 1e9;
                for (const auto& q : outline)
                    baseRadius = std::min(baseRadius, std::hypot(q[0], q[1]));
                lift = doc::MeasureFlatFollowerLift(outline, baseRadius);
                return true;
            }
        error = std::string("engine study: the part with role '") + role + "' has no extruded sketch";
        return false;
    }
    error = std::string("engine study: no part with role '") + role + "' in the document";
    return false;
}

}  // namespace

bool DeriveEngineStudy(const doc::Document& d, EngineStudy& study, std::string& error)
{
    Reader v{d, error};
    sim::engine::EngineDesign& sd = study.simulation;
    sd = sim::engine::EngineDesign();
    const double s = v("s");
    sd.cylinder.geometry.bore = v("bore");
    sd.cylinder.geometry.stroke = v("stroke");
    sd.cylinder.geometry.compressionRatio = v("compression_ratio");
    sd.cylinder.intakeValve.diameter = v("intake_valve_diameter") - 0.004 * s;   // inner seat diameter
    sd.cylinder.exhaustValve.diameter = v("exhaust_valve_diameter") - 0.004 * s;
    sd.rodLength = v("rod_length");
    sd.intake = {v("intake_open"), v("intake_close"), v("intake_lift")};
    sd.exhaust = {v("exhaust_open"), v("exhaust_close"), v("exhaust_lift")};
    sd.ignition.pointsOpenDeg = v("points_open_deg");
    sd.magnetMountDeg = v("magnet_mount_deg");
    sd.loadCoefficient = v("load_coefficient");
    sd.starterStallTorque = v("starter_stall_torque");
    sd.starterFreeSpeedRpm = v("starter_free_speed");
    sd.starterDuration = v("starter_duration");
    study.layout = {v("r"), sd.rodLength, v("spring_length")};
    sim::magneto::MagnetoGeometry& mg = study.magneto;
    mg.flywheelRadius = v("magneto_flywheel_radius");
    mg.assemblyDepth = v("magneto_assembly_depth");
    mg.segmentAngleDeg = v("magneto_segment_angle");
    mg.airGap = v("magneto_air_gap");
    mg.legWidth = v("magneto_leg_width");
    mg.legLength = v("magneto_leg_length");
    mg.yokeThickness = v("magneto_yoke_thickness");
    mg.stackDepth = v("magneto_stack_depth");
    mg.magnetRemanence = v("magneto_remanence");
    if (!v.ok)
        return false;

    // Masses and inertias: from the drawn parts and their materials, per rigid body (body frame).
    study.bodies.clear();
    std::map<std::string, std::vector<kernel::MassProperties>> parts;
    for (const doc::PartInfo& p : d.Parts())
    {
        kernel::MassProperties mp;
        std::string e;
        if (!d.Mass(p.path, mp, e))
        {
            error = "engine study: " + p.name + ": " + e;
            return false;
        }
        if (mp.mass > 0.0)
            parts[p.body].push_back(mp);
    }
    for (auto& [body, list] : parts)
    {
        BodyMass b;
        for (const kernel::MassProperties& mp : list)
        {
            b.mass += mp.mass;
            b.centroid = {b.centroid.x + mp.mass * mp.centroid.x, b.centroid.y + mp.mass * mp.centroid.y, b.centroid.z + mp.mass * mp.centroid.z};
            // Izz about the body Z axis: own Izz + parallel axis.
            b.inertiaZ += mp.inertia[2] + mp.mass * (mp.centroid.x * mp.centroid.x + mp.centroid.y * mp.centroid.y);
        }
        if (b.mass > 0)
            b.centroid = {b.centroid.x / b.mass, b.centroid.y / b.mass, b.centroid.z / b.mass};
        b.inertiaZCentroid = b.inertiaZ - b.mass * (b.centroid.x * b.centroid.x + b.centroid.y * b.centroid.y);
        study.bodies[body] = b;
    }
    for (const char* body : {"Crankshaft", "Rod", "Piston"})
        if (!study.bodies.contains(body))
        {
            error = std::string("engine study: the document has no '") + body + "' body";
            return false;
        }
    sd.crankshaftInertia = study.bodies["Crankshaft"].inertiaZ;
    sd.rodMass = study.bodies["Rod"].mass;
    sd.rodCentreOfMass = study.bodies["Rod"].centroid.x;
    sd.rodInertia = study.bodies["Rod"].inertiaZCentroid;
    sd.pistonMass = study.bodies["Piston"].mass;

    // Valve lift measured on the lobes as drawn.
    double baseIn = 0, baseEx = 0;
    if (!MeasureLobe(d, "intake_lobe", study.intakeLift, baseIn, error) || !MeasureLobe(d, "exhaust_lobe", study.exhaustLift, baseEx, error))
        return false;
    sd.intakeLiftTable = study.intakeLift;
    sd.exhaustLiftTable = study.exhaustLift;
    return true;
}

}  // namespace aiad::app
