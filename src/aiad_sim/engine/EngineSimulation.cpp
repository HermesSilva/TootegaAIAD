#include "EngineSimulation.h"

#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChDirectSolverLS.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace aiad::sim::engine
{

namespace
{

constexpr double kPi = std::numbers::pi;

double WrapDeg180(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a - 180.0;
}

}  // namespace

double ValveEvent::LiftAt(double cycleDeg) const
{
    const double t = std::fmod(cycleDeg - openDeg + 720.0, 720.0);
    const double duration = std::fmod(closeDeg - openDeg + 720.0, 720.0);
    if (t >= duration)
        return 0.0;
    const double s = std::sin(kPi * t / duration);
    return maxLift * s * s;
}

// Crank-slider in the XY plane, crank axis along Z, piston sliding along X with the head at +X
// (TDC when the crank pin is on +X).
struct EngineSimulation::Mechanism
{
    chrono::ChSystemNSC system;
    std::shared_ptr<chrono::ChBody> ground, crank, rod, piston;
    unsigned int crankLoads = 0, pistonLoads = 0;
    double r = 0.0, l = 0.0;

    Mechanism(const EngineDesign& d)
    {
        using namespace chrono;
        r = d.cylinder.geometry.stroke / 2.0;
        l = d.rodLength;
        system.SetGravitationalAcceleration(ChVector3d(0, 0, 0));
        system.SetSolver(chrono_types::make_shared<ChSolverSparseQR>());

        ground = chrono_types::make_shared<ChBody>();
        ground->SetFixed(true);
        system.AddBody(ground);

        // Balanced crankshaft with the flywheel: centre of mass on the axis.
        crank = chrono_types::make_shared<ChBody>();
        crank->SetMass(4.0);
        crank->SetInertiaXX(ChVector3d(d.crankshaftInertia / 2, d.crankshaftInertia / 2, d.crankshaftInertia));
        system.AddBody(crank);

        rod = chrono_types::make_shared<ChBody>();
        rod->SetPos(ChVector3d(r + d.rodCentreOfMass, 0, 0));  // body frame at the centre of mass
        rod->SetMass(d.rodMass);
        rod->SetInertiaXX(ChVector3d(1e-5, d.rodInertia, d.rodInertia));
        system.AddBody(rod);

        piston = chrono_types::make_shared<ChBody>();
        piston->SetPos(ChVector3d(r + l, 0, 0));
        piston->SetMass(d.pistonMass);
        piston->SetInertiaXX(ChVector3d(1e-4, 1e-4, 1e-4));
        system.AddBody(piston);

        auto mainBearing = chrono_types::make_shared<ChLinkLockRevolute>();
        mainBearing->Initialize(crank, ground, ChFrame<>(ChVector3d(0, 0, 0)));
        system.AddLink(mainBearing);
        auto crankPin = chrono_types::make_shared<ChLinkLockRevolute>();
        crankPin->Initialize(crank, rod, ChFrame<>(ChVector3d(r, 0, 0)));
        system.AddLink(crankPin);
        // Point-on-line along Z at the wrist pin (x, y coincide; avoids the planar redundancy).
        auto wristPin = chrono_types::make_shared<ChLinkLockPointLine>();
        wristPin->Initialize(rod, piston, ChFrame<>(ChVector3d(r + l, 0, 0), QuatFromAngleY(-kPi / 2)));
        system.AddLink(wristPin);
        auto bore = chrono_types::make_shared<ChLinkLockPrismatic>();
        bore->Initialize(piston, ground, ChFrame<>(ChVector3d(r + l, 0, 0), QuatFromAngleY(kPi / 2)));
        system.AddLink(bore);

        crankLoads = crank->AddAccumulator();
        pistonLoads = piston->AddAccumulator();
    }

    double PinAngleDeg() const
    {
        const chrono::ChVector3d pin = crank->TransformPointLocalToParent(chrono::ChVector3d(r, 0, 0));
        return std::atan2(pin.y(), pin.x()) * 180.0 / kPi;
    }
};

EngineSimulation::EngineSimulation(const EngineDesign& design, std::vector<magneto::MagnetoSample> magnetoTable)
    : m_design(design), m_cylinder(design.cylinder), m_ignition(std::move(magnetoTable), design.ignition),
      m_mechanism(std::make_unique<Mechanism>(design))
{
    // Start at the overlap TDC (beginning of the intake stroke) with a fresh charge.
    m_crankDeg = 360.0;
    m_previousRawDeg = m_mechanism->PinAngleDeg();
    m_cylinder.Reset(0.0);
}

EngineSimulation::~EngineSimulation() = default;

double EngineSimulation::Time() const
{
    return m_mechanism->system.GetChTime();
}

EngineSimulation::Pose EngineSimulation::CurrentPose() const
{
    const Mechanism& m = *m_mechanism;
    const chrono::ChVector3d pin = m.crank->TransformPointLocalToParent(chrono::ChVector3d(m.r, 0, 0));
    const chrono::ChVector3d wrist = m.piston->GetPos();
    return {m_crankDeg * kPi / 180.0, wrist.x(), std::atan2(wrist.y() - pin.y(), wrist.x() - pin.x())};
}

EngineSample EngineSimulation::Step(double dt)
{
    Mechanism& m = *m_mechanism;
    const CylinderGeometry& g = m_design.cylinder.geometry;
    const double omega = m.crank->GetAngVelParent().z();
    const double rpm = omega * 60.0 / (2.0 * kPi);
    const double time = m.system.GetChTime();

    // Loads from the current state.
    const double gasForce = m_cylinder.PistonForce();
    m.piston->EmptyAccumulator(m.pistonLoads);
    m.piston->AccumulateForce(m.pistonLoads, chrono::ChVector3d(-gasForce, 0, 0), m.piston->GetPos(), false);

    // Friction mean effective pressure (Heywood, SI engines) [bar], N in rpm.
    const double n = std::abs(rpm) / 1000.0;
    const double fmep = (0.97 + 0.15 * n + 0.05 * n * n) * 1e5;
    const double friction = fmep * g.DisplacedVolume() / (4.0 * kPi) * std::tanh(omega / 2.0);
    const double load = m_design.loadCoefficient * omega * std::abs(omega);
    double starter = 0.0;
    if (time < m_design.starterDuration)
        starter = m_design.starterStallTorque * std::max(0.0, 1.0 - rpm / m_design.starterFreeSpeedRpm);
    m.crank->EmptyAccumulator(m.crankLoads);
    m.crank->AccumulateTorque(m.crankLoads, chrono::ChVector3d(0, 0, starter - friction - load), false);

    m.system.DoStepDynamics(dt);

    // Kinematics after the step.
    const double raw = m.PinAngleDeg();
    m_crankDeg += WrapDeg180(raw - m_previousRawDeg);
    m_previousRawDeg = raw;
    const double cycleDeg = std::fmod(std::fmod(m_crankDeg, 720.0) + 720.0, 720.0);

    CylinderInputs in;
    in.pistonFromTdc = (m.r + m.l) - m.piston->GetPos().x();
    in.pistonSpeed = -m.piston->GetPosDt().x();
    auto lift = [cycleDeg](const std::vector<double>& table, const ValveEvent& event) {
        if (table.size() != 720)
            return event.LiftAt(cycleDeg);
        const int i = int(cycleDeg) % 720;
        const double t = cycleDeg - std::floor(cycleDeg);
        return table[i] + t * (table[(i + 1) % 720] - table[i]);
    };
    in.intakeLift = lift(m_design.intakeLiftTable, m_design.intake);
    in.exhaustLift = lift(m_design.exhaustLiftTable, m_design.exhaust);
    in.crankSpeed = m.crank->GetAngVelParent().z() * 180.0 / kPi;

    const double magnetDeg = WrapDeg180(m_crankDeg - m_design.magnetMountDeg);
    const magneto::IgnitionStep ignition = m_ignition.Step(dt, magnetDeg, m_cylinder.State().pressure);
    in.spark = ignition.spark;
    m_cylinder.Step(dt, in);

    EngineSample s;
    s.time = m.system.GetChTime();
    s.crankDeg = m_crankDeg;
    s.rpm = m.crank->GetAngVelParent().z() * 60.0 / (2.0 * kPi);
    s.pressure = m_cylinder.State().pressure;
    s.volume = m_cylinder.State().volume;
    const double crankOmega = m.crank->GetAngVelParent().z();
    s.gasTorque = std::abs(crankOmega) > 1e-6 ? m_cylinder.PistonForce() * in.pistonSpeed / crankOmega : 0.0;
    s.intakeLift = in.intakeLift;
    s.exhaustLift = in.exhaustLift;
    s.secondaryPeak = ignition.secondaryPeak;
    s.spark = ignition.spark;
    s.combustion = m_cylinder.State().timeSinceSpark >= 0.0;
    return s;
}

}  // namespace aiad::sim::engine
