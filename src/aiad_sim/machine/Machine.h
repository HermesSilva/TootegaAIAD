// A machine as drawn: the multibody mechanism plus the domain elements tied to its joints. No machine is
// known here; the description comes from the document (aiad_app reads and checks it):
//   - chamber: a gas volume closed by a sliding piston (0D cycle, EngineCylinder): its volume follows the
//     piston joint, its valves follow their joints, its pressure pushes the piston;
//   - magneto: a magnet turning past a stator core (field table from MFEM, breaker-point circuit): its
//     angle follows the rotor joint, its spark ignites the chamber it fires.
#pragma once

#include "engine/EngineCylinder.h"
#include "magneto/MagnetoIgnition.h"
#include "mechanism/Multibody.h"

#include <memory>
#include <string>
#include <vector>

namespace aiad::sim::machine
{

struct ChamberValve
{
    int joint = -1;
    double sign = -1.0;        // lift = max(0, sign * joint coordinate)
};

struct Chamber
{
    std::string name;
    int piston = -1;           // prismatic joint of the piston in the cylinder
    // Crown-to-head distance = gapAtRest - sign * piston coordinate; the piston force acts along -sign.
    double sign = 1.0, gapAtRest = 0.0;
    // From the drawing: geometry.bore, geometry.clearance, the valve diameters (summed per kind).
    engine::CylinderParameters cylinder;
    std::vector<ChamberValve> intake, exhaust;
    int ignition = -1;         // index of the magneto that fires it (-1: none)
    int crank = -1;            // joint whose speed scales the burn duration (-1: the ignition rotor)
};

struct Magneto
{
    std::string name;
    int rotor = -1;            // revolute joint turning the magnet
    // Magnet angle from the core axis [deg] = atRestDeg + sign * rotor coordinate.
    double atRestDeg = 0.0, sign = 1.0;
    magneto::MagnetoGeometry geometry;
    magneto::IgnitionCircuit circuit;
};

// A dynamometer on a shaft (a revolute joint): an absorber that brakes it, as an eddy-current bench brake does, and
// measures the torque it takes. Speed: holds the shaft at `speed` (absorbing only: below it, it lets go) and reads the
// torque the machine gives there; sweep: the held speed goes from `from` to `to` in `seconds` (a power curve); torque:
// a constant braking torque, the speed is where the machine settles. Engaged from `start` [s].
struct Dynamometer
{
    std::string name;
    int joint = -1;
    enum class Mode
    {
        Speed,
        Sweep,
        Torque
    } mode = Mode::Speed;
    double speed = 0.0, from = 0.0, to = 0.0, seconds = 0.0;   // [rad/s], [s]
    double torque = 0.0, maxTorque = 1.0e4;                    // [N m]
    double start = 0.0;                                        // [s]
    double inertia = 0.0;                                      // of the shaft about its axis [kg m^2] (the controller's gains)
};

struct Description
{
    mb::Model mechanism;
    std::vector<Chamber> chambers;
    std::vector<Magneto> magnetos;
    std::vector<Dynamometer> dynamometers;
};

struct ChamberState
{
    double pressure = 0.0, volume = 0.0, temperature = 0.0;   // [Pa], [m^3], [K]
    double intakeLift = 0.0, exhaustLift = 0.0;               // largest valve lift of each kind [m]
    double gasForce = 0.0;                                    // on the piston [N]
    bool combustion = false;
    int combustions = 0;
    double work = 0.0;                                        // indicated work since the start [J]
    double minVolume = 1e30, maxVolume = 0.0, peakPressure = 0.0;
};

struct DynamometerState
{
    double target = 0.0;    // held speed [rad/s] (0: not engaged)
    double torque = 0.0;    // absorbed [N m], >= 0
    double speed = 0.0;     // of the shaft [rad/s], >= 0
    double power = 0.0;     // torque x speed [W]
    double energy = 0.0;    // absorbed since the start [J]
    double integral = 0.0;  // the controller's
};

struct MagnetoState
{
    double angleDeg = 0.0;           // magnet from the core axis
    double lastSecondaryKv = 0.0;    // secondary peak at the last points opening
    bool spark = false;              // in the last step
    int sparks = 0;
};

class Machine
{
public:
    // tables: one field table per magneto (magneto::ComputeMagnetoTable of its geometry).
    Machine(const Description& description, std::vector<std::vector<magneto::MagnetoSample>> tables);
    ~Machine();

    void Step(double dt);
    double Time() const;
    // The step the mechanism and the gas need [s].
    double StepSize() const;
    const Description& Describe() const { return m_description; }
    const mb::Multibody& Mechanism() const { return *m_mechanism; }
    const ChamberState& Chamber(size_t i) const { return m_chambers[i]; }
    const MagnetoState& Magneto(size_t i) const { return m_magnetos[i]; }
    const DynamometerState& Dyno(size_t i) const { return m_dynos[i]; }

private:
    Description m_description;
    std::unique_ptr<mb::Multibody> m_mechanism;
    std::vector<engine::EngineCylinder> m_cylinders;
    std::vector<magneto::MagnetoIgnition> m_ignitions;
    std::vector<ChamberState> m_chambers;
    std::vector<MagnetoState> m_magnetos;
    std::vector<DynamometerState> m_dynos;
};

}  // namespace aiad::sim::machine
