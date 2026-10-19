// Single-cylinder four-stroke engine: mechanism (Project Chrono) + 0D cylinder + magneto ignition.
//
// The crankshaft (with the flywheel and its magnet) turns freely on its bearing. Every step the crank
// angle gives the piston position, the valve lifts and the magnet position; the cylinder returns the
// gas pressure, applied as a force on the piston; the magneto decides when the plug fires. Friction
// (FMEP), a propeller-like load and a starter motor act on the crankshaft.
#pragma once

#include "engine/EngineCylinder.h"
#include "magneto/MagnetoIgnition.h"

#include <functional>
#include <memory>
#include <vector>

namespace aiad::sim::engine
{

struct ValveEvent
{
    double openDeg = 0.0;     // crank angle in the 720-degree cycle (0 = firing TDC)
    double closeDeg = 0.0;
    double maxLift = 0.008;   // [m]
    double LiftAt(double cycleDeg) const;
};

struct EngineDesign
{
    CylinderParameters cylinder;
    double rodLength = 0.100;                  // [m]
    // Masses and inertias. In the application they come from the drawn parts (aiad_model); the
    // defaults below are only for standalone tests.
    double crankshaftInertia = 0.03;           // rotating assembly about the crank axis [kg m^2]
    double rodMass = 0.35, rodInertia = 4e-4;  // [kg], [kg m^2] about its centre of mass (crank axis direction)
    double rodCentreOfMass = 0.035;            // distance from the crank pin along the rod [m]
    double pistonMass = 0.30;                  // [kg] (piston, pin, rings)
    // Valve timing (kinematic cams, 2:1 camshaft). When the lift tables are present (one value per
    // cycle degree, measured on the drawn cam lobes) they replace the analytic events.
    ValveEvent intake{350.0, 580.0, 0.008};
    ValveEvent exhaust{140.0, 370.0, 0.008};
    std::vector<double> intakeLiftTable, exhaustLiftTable;
    // Ignition: crank angle at which the magnet sits on the core axis, so that the points open
    // at (mount + pointsOpenDeg) every revolution.
    magneto::IgnitionCircuit ignition;
    double magnetMountDeg = 0.0;
    // Crankshaft loads.
    double loadCoefficient = 2.5e-4;           // propeller load T = k * omega^2 [N m s^2]
    // Starter motor (DC characteristic at the crank, through its reduction gear):
    // T = stall * (1 - omega / free speed), never negative.
    double starterStallTorque = 50.0;          // [N m]
    double starterFreeSpeedRpm = 800.0;
    double starterDuration = 1.0;              // [s]
};

struct EngineSample
{
    double time = 0.0;
    double crankDeg = 0.0;         // unwrapped crank angle [deg]
    double rpm = 0.0;
    double pressure = 0.0;         // cylinder [Pa]
    double volume = 0.0;           // cylinder [m^3]
    double gasTorque = 0.0;        // torque of the gas force on the crank [N m]
    double intakeLift = 0.0;       // [m]
    double exhaustLift = 0.0;      // [m]
    double secondaryPeak = 0.0;    // magneto secondary voltage at a points opening [V]
    bool spark = false;
    bool combustion = false;       // a combustion is running
};

class EngineSimulation
{
public:
    EngineSimulation(const EngineDesign& design, std::vector<magneto::MagnetoSample> magnetoTable);
    ~EngineSimulation();

    // Advance by dt [s].
    EngineSample Step(double dt);

    double Time() const;
    const EngineDesign& Design() const { return m_design; }
    const EngineCylinder& Cylinder() const { return m_cylinder; }

    // Rigid-body poses for visualisation: crank angle [rad] and piston/rod placement.
    struct Pose
    {
        double crankAngle = 0.0;
        double pistonX = 0.0;
        double rodAngle = 0.0;
    };
    Pose CurrentPose() const;

private:
    struct Mechanism;
    EngineDesign m_design;
    EngineCylinder m_cylinder;
    magneto::MagnetoIgnition m_ignition;
    std::unique_ptr<Mechanism> m_mechanism;
    double m_crankDeg = 0.0;       // unwrapped
    double m_previousRawDeg = 0.0;
};

}  // namespace aiad::sim::engine
