// Breaker-point magneto ignition (lumped circuit) driven by the magnetostatic table Phi(angle), L(angle).
//
// Points closed: the primary is short-circuited and carries the current induced by the moving magnet,
//   d(L1 i)/dt + R1 i = -N1 dPhi/dt,  L1 = N1^2 * (L / N^2)(angle).
// Points open: the primary energy 1/2 L1 i^2 rings into the condenser and the secondary winding
// capacitance (referred to the primary by n^2, n = N2/N1); the primary peaks at
//   V1 = i sqrt(L1 / (C + Cs n^2)) and the secondary at V2 = k * V1 * n (k: coil coupling factor).
// A spark occurs if V2 exceeds the plug breakdown voltage, which grows with the cylinder pressure.
#pragma once

#include "magneto/MagnetoField.h"

#include <vector>

namespace aiad::sim::magneto
{

struct IgnitionCircuit
{
    int primaryTurns = 200;
    int secondaryTurns = 12000;
    double primaryResistance = 0.6;      // [ohm]
    double condenser = 0.2e-6;           // [F]
    double secondaryCapacitance = 40e-12;  // secondary winding + lead + plug [F]
    double couplingFactor = 0.9;         // magnetic coupling of primary and secondary
    double pointsCloseDeg = -30.0;       // magnet angle (relative to the core axis) where the points close
    double pointsOpenDeg = 8.0;          // ... and where they open (the "E-gap")
    // Plug breakdown: V = base + slope * cylinder pressure (Paschen trend at the gap) [V, V/Pa].
    double breakdownBase = 2000.0;
    double breakdownSlope = 5.5e-3;
};

struct IgnitionStep
{
    bool spark = false;
    double primaryCurrent = 0.0;         // [A]
    double secondaryPeak = 0.0;          // [V] (only on the opening step)
    double breakdownVoltage = 0.0;       // [V] (only on the opening step)
};

class MagnetoIgnition
{
public:
    MagnetoIgnition(std::vector<MagnetoSample> table, const IgnitionCircuit& circuit);

    // Advance by dt; magnetAngleDeg is the magnet assembly position relative to the core axis
    // (wrapped to (-180, 180]); cylinderPressure sets the plug breakdown voltage.
    IgnitionStep Step(double dt, double magnetAngleDeg, double cylinderPressure);

    double Flux(double angleDeg) const;
    double PrimaryInductance(double angleDeg) const;
    const IgnitionCircuit& Circuit() const { return m_circuit; }

private:
    double Interpolate(double angleDeg, bool flux) const;
    bool PointsClosed(double angleDeg) const;

    std::vector<MagnetoSample> m_table;
    IgnitionCircuit m_circuit;
    double m_linkage = 0.0;              // L1 i [Wb-turn]
    double m_previousAngle = 0.0;
    bool m_previousClosed = false;
    bool m_started = false;
};

// Secondary peak voltage of one pass of the magnet at constant speed with the given circuit [V].
double SecondaryPeakAt(const std::vector<MagnetoSample>& table, const IgnitionCircuit& circuit, double rpm);

// Points opening angle (whole degrees in [-20, 30]) that maximises the secondary voltage at rpm.
double OptimalPointsOpening(const std::vector<MagnetoSample>& table, IgnitionCircuit circuit, double rpm);

}  // namespace aiad::sim::magneto
