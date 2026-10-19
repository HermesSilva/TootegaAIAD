// Zero-dimensional (single-zone) thermodynamic model of one engine cylinder.
//
// The cylinder is an open system of ideal gas exchanging mass with an intake and an exhaust plenum
// through two valves, burning fuel with a Wiebe heat-release law after the spark, and losing heat
// to the walls (Woschni). The mechanism (multibody solver) provides the piston position and the
// valve lifts; the model returns the gas pressure that pushes the piston.
#pragma once

#include <cstdint>

namespace aiad::sim::engine
{

// Ideal gas with a temperature-dependent heat capacity ratio gamma(T) = gamma0 - gammaSlope * T
// (single-zone engine-gas fit). gammaSlope = 0 gives a calorically perfect gas.
// With c = gamma0 - 1 and b = gammaSlope: cv(T) = R / (c - b T) and u(T) = -(R/b) ln(1 - b T / c).
struct GasProperties
{
    double R = 287.0;             // specific gas constant [J/(kg K)]
    double gamma0 = 1.392;
    double gammaSlope = 8.13e-5;  // [1/K]

    double Gamma(double T) const { return gamma0 - gammaSlope * T; }
    double Cv(double T) const { return R / (Gamma(T) - 1.0); }
    double Cp(double T) const { return Gamma(T) * R / (Gamma(T) - 1.0); }
    double InternalEnergy(double T) const;           // specific, zero at 0 K [J/kg]
    double TemperatureFromEnergy(double u) const;    // inverse of InternalEnergy
    double Enthalpy(double T) const { return InternalEnergy(T) + R * T; }
};

struct CylinderGeometry
{
    double bore = 0.080;              // [m]
    double stroke = 0.060;            // [m] (2 x crank radius)
    double compressionRatio = 10.0;   // (Vd + Vc) / Vc
    // A drawn chamber: the volume at pistonFromTdc = 0 [m^3] (pistonFromTdc is then the crown's distance
    // from the head), replacing Vd / (CR - 1). Negative = use the compression ratio.
    double clearance = -1.0;
    double PistonArea() const;
    double DisplacedVolume() const;
    double ClearanceVolume() const;
};

struct Valve
{
    double diameter = 0.030;          // inner seat diameter [m]
    double dischargeCoefficient = 0.7;
    double maxCurtainLiftRatio = 0.25; // curtain area saturates at lift = ratio * diameter (port limit)
    double FlowArea(double lift) const;
};

struct Plenum
{
    double pressure = 101325.0;       // [Pa]
    double temperature = 300.0;       // [K]
};

struct Combustion
{
    double fuelLowerHeatingValue = 44.0e6;  // [J/kg] gasoline
    double airFuelRatio = 14.7;             // mass of air per mass of fuel (fresh charge)
    double combustionEfficiency = 0.95;
    double wiebeA = 5.0;                    // completeness parameter (x_b -> 1 - e^-a)
    double wiebeM = 2.0;                    // shape exponent
    // Burn duration in crank degrees: turbulent flame speed scales with engine speed, so the
    // combustion takes a roughly constant crank angle (not a constant time).
    double durationDeg = 45.0;
    // A spark ignites only if the fuel fraction is at least this share of the fresh-charge fraction
    // (lean limit); a magneto also fires at the exhaust TDC, into burned gas.
    double leanLimit = 0.5;
};

struct HeatTransfer
{
    bool enabled = true;
    double wallTemperature = 450.0;   // [K]
    double woschniC1 = 2.28;          // gas-velocity coefficient (compression/expansion)
};

struct CylinderParameters
{
    CylinderGeometry geometry;
    GasProperties gas;
    Valve intakeValve;
    Valve exhaustValve{0.026, 0.7, 0.25};
    Plenum intake;
    Plenum exhaust{105000.0, 800.0};
    Combustion combustion;
    HeatTransfer heat;
};

// Inputs given by the mechanism at each step.
struct CylinderInputs
{
    double pistonFromTdc = 0.0;    // piston distance below top dead centre [m] (0 at TDC, stroke at BDC)
    double pistonSpeed = 0.0;      // d(pistonFromTdc)/dt [m/s]
    double intakeLift = 0.0;       // [m]
    double exhaustLift = 0.0;      // [m]
    bool spark = false;            // true on the step the spark plug fires
    double crankSpeed = 0.0;       // [deg/s], sets the burn duration in time
};

struct CylinderState
{
    double mass = 0.0;            // gas mass in the cylinder [kg]
    double temperature = 300.0;   // [K]
    double volume = 0.0;          // [m^3]
    double pressure = 101325.0;   // [Pa]
    double fuelMass = 0.0;        // unburned fuel trapped for this cycle [kg]
    double burnedFraction = 0.0;  // Wiebe x_b of the current combustion
    double timeSinceSpark = -1.0; // [s]; negative when no combustion is running
};

// Per-step energy bookkeeping (positive = into the gas).
struct CylinderFluxes
{
    double work = 0.0;            // -p dV [J]
    double heatReleased = 0.0;    // combustion [J]
    double heatLost = 0.0;        // to the walls [J] (positive = lost)
    double massIn = 0.0;          // through the intake valve [kg] (negative = backflow)
    double massOut = 0.0;         // through the exhaust valve [kg] (negative = backflow)
};

class EngineCylinder
{
public:
    explicit EngineCylinder(const CylinderParameters& parameters);

    // Fill the cylinder with intake-plenum gas at the given piston position.
    void Reset(double pistonFromTdc);

    // Advance the gas state by dt with the mechanism inputs; returns the fluxes of the step.
    CylinderFluxes Step(double dt, const CylinderInputs& inputs);

    // Gas force on the piston crown along the cylinder axis, pushing the piston away from the head [N]
    // (net of the crankcase pressure).
    double PistonForce(double crankcasePressure = 101325.0) const;

    const CylinderState& State() const { return m_state; }
    const CylinderParameters& Parameters() const { return m_parameters; }
    double Volume(double pistonFromTdc) const;

    // Override the trapped fuel (tests: closed-cycle runs without an intake stroke).
    void SetFuelMass(double fuelMass) { m_state.fuelMass = fuelMass; }

private:
    struct Rates
    {
        double massDot = 0.0, energyDot = 0.0, fuelDot = 0.0;
        double workDot = 0.0, heatReleaseDot = 0.0, heatLossDot = 0.0, intakeDot = 0.0, exhaustDot = 0.0;
    };
    Rates Derivatives(double mass, double temperature, double fuel, double volume, double volumeDot,
                      const CylinderInputs& inputs, double burnRate) const;
    // Signed mass flow into the cylinder through a valve [kg/s]; enthalpy flow returned in enthalpyDot.
    double ValveMassFlow(const Valve& valve, double lift, const Plenum& plenum, double cylinderPressure,
                         double cylinderTemperature, double& enthalpyDot) const;
    double WiebeFraction(double timeSinceSpark) const;

    CylinderParameters m_parameters;
    CylinderState m_state;
    double m_fuelAtSpark = 0.0;        // fuel trapped when the spark fired [kg]
    double m_burnSeconds = 0.0;        // duration of the running combustion [s]
    double m_meanPistonSpeed = 0.0;    // running mean of |piston speed| (Woschni) [m/s]
};

}  // namespace aiad::sim::engine
