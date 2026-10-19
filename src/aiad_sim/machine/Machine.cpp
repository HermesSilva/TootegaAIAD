#include "Machine.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace aiad::sim::machine
{

namespace
{

constexpr double kDeg = 180.0 / std::numbers::pi;

double Wrap180(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a - 180.0;
}

}  // namespace

Machine::Machine(const Description& description, std::vector<std::vector<magneto::MagnetoSample>> tables)
    : m_description(description), m_mechanism(std::make_unique<mb::Multibody>(description.mechanism))
{
    for (const machine::Chamber& c : m_description.chambers)
    {
        m_cylinders.emplace_back(c.cylinder);
        m_cylinders.back().Reset(c.gapAtRest);
        ChamberState s;
        s.volume = m_cylinders.back().State().volume;
        s.pressure = m_cylinders.back().State().pressure;
        s.temperature = m_cylinders.back().State().temperature;
        m_chambers.push_back(s);
    }
    for (size_t i = 0; i < m_description.magnetos.size(); ++i)
    {
        m_ignitions.emplace_back(i < tables.size() ? tables[i] : std::vector<magneto::MagnetoSample>(), m_description.magnetos[i].circuit);
        m_magnetos.push_back({});
    }
}

Machine::~Machine() = default;

double Machine::Time() const
{
    return m_mechanism->Time();
}

double Machine::StepSize() const
{
    // The gas exchange and the combustion need about 2e-5 s (0.4 crank degree at 3500 rpm).
    return std::min(m_mechanism->StableStep(), m_description.chambers.empty() ? 2.0e-4 : 2.0e-5);
}

void Machine::Step(double dt)
{
    mb::Multibody& mb = *m_mechanism;
    // Gas pressure on the pistons, from the current state.
    for (size_t i = 0; i < m_cylinders.size(); ++i)
    {
        const machine::Chamber& c = m_description.chambers[i];
        const double force = m_cylinders[i].PistonForce();
        m_chambers[i].gasForce = force;
        mb.ApplyJointLoad(c.piston, -c.sign * force);
    }
    mb.Step(dt);

    // Magnetos: magnet angle from their rotor; the spark goes to the chamber each one fires.
    std::vector<bool> spark(m_cylinders.size(), false);
    for (size_t k = 0; k < m_ignitions.size(); ++k)
    {
        const machine::Magneto& g = m_description.magnetos[k];
        MagnetoState& s = m_magnetos[k];
        s.angleDeg = Wrap180(g.atRestDeg + g.sign * mb.Coordinate(g.rotor) * kDeg);
        double pressure = 101325.0;
        for (size_t i = 0; i < m_cylinders.size(); ++i)
            if (m_description.chambers[i].ignition == int(k))
                pressure = m_cylinders[i].State().pressure;
        const magneto::IgnitionStep step = m_ignitions[k].Step(dt, s.angleDeg, pressure);
        s.spark = step.spark;
        if (step.secondaryPeak > 0.0)
            s.lastSecondaryKv = step.secondaryPeak / 1e3;
        if (step.spark)
        {
            ++s.sparks;
            for (size_t i = 0; i < m_cylinders.size(); ++i)
                spark[i] = spark[i] || m_description.chambers[i].ignition == int(k);
        }
    }

    // Chambers: volume from the piston, flow areas from the valves.
    for (size_t i = 0; i < m_cylinders.size(); ++i)
    {
        const machine::Chamber& c = m_description.chambers[i];
        ChamberState& s = m_chambers[i];
        engine::CylinderInputs in;
        in.pistonFromTdc = c.gapAtRest - c.sign * mb.Coordinate(c.piston);
        in.pistonSpeed = -c.sign * mb.Rate(c.piston);
        auto lift = [&](const std::vector<ChamberValve>& valves) {
            double most = 0.0;
            for (const ChamberValve& v : valves)
                most = std::max(most, v.sign * mb.Coordinate(v.joint));
            return most;
        };
        // The 0D model has one valve of each kind: the largest lift (the seat diameters are summed).
        in.intakeLift = s.intakeLift = lift(c.intake);
        in.exhaustLift = s.exhaustLift = lift(c.exhaust);
        in.spark = spark[i];
        const int crank = c.crank >= 0 ? c.crank : c.ignition >= 0 ? m_description.magnetos[size_t(c.ignition)].rotor : -1;
        in.crankSpeed = crank >= 0 ? mb.Rate(crank) * kDeg : 0.0;
        const double before = m_cylinders[i].State().volume;
        const bool burning = m_cylinders[i].State().timeSinceSpark >= 0.0;
        m_cylinders[i].Step(dt, in);
        const engine::CylinderState& st = m_cylinders[i].State();
        s.work += 0.5 * (s.pressure + st.pressure) * (st.volume - before);
        s.pressure = st.pressure;
        s.volume = st.volume;
        s.temperature = st.temperature;
        s.combustion = st.timeSinceSpark >= 0.0;
        s.combustions += s.combustion && !burning ? 1 : 0;
        s.minVolume = std::min(s.minVolume, st.volume);
        s.maxVolume = std::max(s.maxVolume, st.volume);
        s.peakPressure = std::max(s.peakPressure, st.pressure);
    }
}

}  // namespace aiad::sim::machine
