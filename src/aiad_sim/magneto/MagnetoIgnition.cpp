#include "MagnetoIgnition.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace aiad::sim::magneto
{

MagnetoIgnition::MagnetoIgnition(std::vector<MagnetoSample> table, const IgnitionCircuit& circuit)
    : m_table(std::move(table)), m_circuit(circuit)
{
    if (m_table.size() < 2)
        throw std::invalid_argument("magneto: the field table needs at least two samples");
}

double MagnetoIgnition::Interpolate(double angleDeg, bool flux) const
{
    // Outside the computed window the magnet is far from the core: hold the edge values.
    const double a = std::clamp(angleDeg, m_table.front().angleDeg, m_table.back().angleDeg);
    const auto upper = std::lower_bound(m_table.begin(), m_table.end(), a,
                                        [](const MagnetoSample& s, double v) { return s.angleDeg < v; });
    if (upper == m_table.begin())
        return flux ? upper->fluxPerTurn : upper->inductancePerTurnSquared;
    const auto lower = upper - 1;
    const double t = (a - lower->angleDeg) / (upper->angleDeg - lower->angleDeg);
    const double v0 = flux ? lower->fluxPerTurn : lower->inductancePerTurnSquared;
    const double v1 = flux ? upper->fluxPerTurn : upper->inductancePerTurnSquared;
    return v0 + t * (v1 - v0);
}

double MagnetoIgnition::Flux(double angleDeg) const
{
    return Interpolate(angleDeg, true);
}

double MagnetoIgnition::PrimaryInductance(double angleDeg) const
{
    const double n = m_circuit.primaryTurns;
    return n * n * Interpolate(angleDeg, false);
}

bool MagnetoIgnition::PointsClosed(double angleDeg) const
{
    return angleDeg >= m_circuit.pointsCloseDeg && angleDeg < m_circuit.pointsOpenDeg;
}

IgnitionStep MagnetoIgnition::Step(double dt, double magnetAngleDeg, double cylinderPressure)
{
    IgnitionStep out;
    const bool closed = PointsClosed(magnetAngleDeg);
    if (!m_started)
    {
        m_started = true;
        m_previousAngle = magnetAngleDeg;
        m_previousClosed = closed;
        return out;
    }

    const double n1 = m_circuit.primaryTurns;
    const double inductance = PrimaryInductance(magnetAngleDeg);
    if (closed)
    {
        // Backward Euler on the linkage: lambda' = e - R lambda / L, e = -N1 dPhi/dt.
        const double emf = -n1 * (Flux(magnetAngleDeg) - Flux(m_previousAngle)) / dt;
        if (!m_previousClosed)
            m_linkage = 0.0;
        m_linkage = (m_linkage + dt * emf) / (1.0 + dt * m_circuit.primaryResistance / inductance);
        out.primaryCurrent = m_linkage / inductance;
    }
    else if (m_previousClosed)
    {
        // Points just opened: the interrupted current rings into the condenser.
        const double current = m_linkage / inductance;
        const double ratio = m_circuit.secondaryTurns / n1;
        const double capacitance = m_circuit.condenser + m_circuit.secondaryCapacitance * ratio * ratio;
        const double primaryPeak = std::abs(current) * std::sqrt(inductance / capacitance);
        out.secondaryPeak = m_circuit.couplingFactor * primaryPeak * ratio;
        out.breakdownVoltage = m_circuit.breakdownBase + m_circuit.breakdownSlope * cylinderPressure;
        out.spark = out.secondaryPeak >= out.breakdownVoltage;
        m_linkage = 0.0;
    }

    m_previousAngle = magnetAngleDeg;
    m_previousClosed = closed;
    return out;
}

double SecondaryPeakAt(const std::vector<MagnetoSample>& table, const IgnitionCircuit& circuit, double rpm)
{
    MagnetoIgnition ignition(table, circuit);
    const double stepDeg = 0.02, dt = stepDeg / (rpm * 6.0);
    double peak = 0.0;
    for (double a = table.front().angleDeg; a <= table.back().angleDeg; a += stepDeg)
        peak = std::max(peak, ignition.Step(dt, a, 0.0).secondaryPeak);
    return peak;
}

double OptimalPointsOpening(const std::vector<MagnetoSample>& table, IgnitionCircuit circuit, double rpm)
{
    double best = circuit.pointsOpenDeg, bestVoltage = -1.0;
    for (double open = -20.0; open <= 30.0; open += 1.0)
    {
        circuit.pointsOpenDeg = open;
        const double v = SecondaryPeakAt(table, circuit, rpm);
        if (v > bestVoltage)
        {
            bestVoltage = v;
            best = open;
        }
    }
    return best;
}

}  // namespace aiad::sim::magneto
