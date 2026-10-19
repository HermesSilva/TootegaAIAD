#include "EngineCylinder.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace aiad::sim::engine
{

double GasProperties::InternalEnergy(double T) const
{
    const double c = gamma0 - 1.0;
    if (gammaSlope == 0.0)
        return R * T / c;
    return -(R / gammaSlope) * std::log(1.0 - gammaSlope * T / c);
}

double GasProperties::TemperatureFromEnergy(double u) const
{
    const double c = gamma0 - 1.0;
    if (gammaSlope == 0.0)
        return u * c / R;
    return (c / gammaSlope) * (1.0 - std::exp(-gammaSlope * u / R));
}

double CylinderGeometry::PistonArea() const
{
    return std::numbers::pi * bore * bore / 4.0;
}

double CylinderGeometry::DisplacedVolume() const
{
    return PistonArea() * stroke;
}

double CylinderGeometry::ClearanceVolume() const
{
    return DisplacedVolume() / (compressionRatio - 1.0);
}

double Valve::FlowArea(double lift) const
{
    const double effectiveLift = std::clamp(lift, 0.0, maxCurtainLiftRatio * diameter);
    return std::numbers::pi * diameter * effectiveLift;
}

EngineCylinder::EngineCylinder(const CylinderParameters& parameters) : m_parameters(parameters)
{
    Reset(0.0);
}

double EngineCylinder::Volume(double pistonFromTdc) const
{
    const CylinderGeometry& g = m_parameters.geometry;
    return g.ClearanceVolume() + g.PistonArea() * pistonFromTdc;
}

void EngineCylinder::Reset(double pistonFromTdc)
{
    const GasProperties& gas = m_parameters.gas;
    m_state = {};
    m_state.volume = Volume(pistonFromTdc);
    m_state.pressure = m_parameters.intake.pressure;
    m_state.temperature = m_parameters.intake.temperature;
    m_state.mass = m_state.pressure * m_state.volume / (gas.R * m_state.temperature);
    m_state.fuelMass = m_state.mass / (1.0 + m_parameters.combustion.airFuelRatio);
    m_fuelAtSpark = 0.0;
    m_meanPistonSpeed = 0.0;
}

double EngineCylinder::PistonForce(double crankcasePressure) const
{
    return (m_state.pressure - crankcasePressure) * m_parameters.geometry.PistonArea();
}

double EngineCylinder::WiebeFraction(double timeSinceSpark) const
{
    const Combustion& c = m_parameters.combustion;
    if (timeSinceSpark <= 0.0 || m_burnSeconds <= 0.0)
        return 0.0;
    const double tau = std::min(timeSinceSpark / m_burnSeconds, 1.0);
    return 1.0 - std::exp(-c.wiebeA * std::pow(tau, c.wiebeM + 1.0));
}

double EngineCylinder::ValveMassFlow(const Valve& valve, double lift, const Plenum& plenum, double cylinderPressure,
                                     double cylinderTemperature, double& enthalpyDot) const
{
    enthalpyDot = 0.0;
    const double area = valve.dischargeCoefficient * valve.FlowArea(lift);
    if (area <= 0.0 || plenum.pressure == cylinderPressure)
        return 0.0;

    const GasProperties& gas = m_parameters.gas;
    const bool inflow = plenum.pressure > cylinderPressure;
    const double pUp = inflow ? plenum.pressure : cylinderPressure;
    const double tUp = inflow ? plenum.temperature : cylinderTemperature;
    const double pDown = inflow ? cylinderPressure : plenum.pressure;
    const double g = gas.Gamma(tUp);

    // Isentropic compressible orifice flow, choked below the critical pressure ratio.
    const double ratio = std::max(pDown / pUp, 0.0);
    const double critical = std::pow(2.0 / (g + 1.0), g / (g - 1.0));
    double psi;
    if (ratio <= critical)
        psi = std::sqrt(g) * std::pow(2.0 / (g + 1.0), (g + 1.0) / (2.0 * (g - 1.0)));
    else
        psi = std::sqrt(2.0 * g / (g - 1.0) * (std::pow(ratio, 2.0 / g) - std::pow(ratio, (g + 1.0) / g)));
    const double flow = area * pUp / std::sqrt(gas.R * tUp) * psi;

    enthalpyDot = (inflow ? 1.0 : -1.0) * flow * gas.Enthalpy(tUp);
    return inflow ? flow : -flow;
}

EngineCylinder::Rates EngineCylinder::Derivatives(double mass, double temperature, double fuel, double volume,
                                                   double volumeDot, const CylinderInputs& inputs,
                                                   double burnRate) const
{
    const CylinderParameters& p = m_parameters;
    const GasProperties& gas = p.gas;
    const double pressure = mass * gas.R * temperature / volume;

    Rates r;
    double hIntake = 0.0, hExhaust = 0.0;
    r.intakeDot = ValveMassFlow(p.intakeValve, inputs.intakeLift, p.intake, pressure, temperature, hIntake);
    r.exhaustDot = -ValveMassFlow(p.exhaustValve, inputs.exhaustLift, p.exhaust, pressure, temperature, hExhaust);

    r.massDot = r.intakeDot - r.exhaustDot;
    // Fresh charge brings fuel; outflow (intake backflow or exhaust) carries the current fuel fraction;
    // exhaust backflow is burned gas without fuel.
    const double fuelFraction = mass > 0.0 ? fuel / mass : 0.0;
    const double freshFuelFraction = 1.0 / (1.0 + p.combustion.airFuelRatio);
    r.fuelDot = (r.intakeDot > 0.0 ? r.intakeDot * freshFuelFraction : r.intakeDot * fuelFraction) -
                std::max(r.exhaustDot, 0.0) * fuelFraction;

    r.heatReleaseDot = burnRate * p.combustion.fuelLowerHeatingValue * p.combustion.combustionEfficiency;
    r.fuelDot -= burnRate;

    if (p.heat.enabled)
    {
        // Woschni: h = 3.26 B^-0.2 p[kPa]^0.8 T^-0.55 w^0.8, w = C1 * mean piston speed.
        const double w = std::max(p.heat.woschniC1 * m_meanPistonSpeed, 0.1);
        const double h = 3.26 * std::pow(p.geometry.bore, -0.2) * std::pow(pressure / 1000.0, 0.8) *
                         std::pow(temperature, -0.55) * std::pow(w, 0.8);
        const double area = 2.0 * p.geometry.PistonArea() +
                            std::numbers::pi * p.geometry.bore * (volume / p.geometry.PistonArea());
        r.heatLossDot = h * area * (temperature - p.heat.wallTemperature);
    }

    r.workDot = -pressure * volumeDot;
    r.energyDot = r.workDot + r.heatReleaseDot - r.heatLossDot + hIntake + hExhaust;
    return r;
}

CylinderFluxes EngineCylinder::Step(double dt, const CylinderInputs& inputs)
{
    const CylinderParameters& p = m_parameters;
    const GasProperties& gas = p.gas;
    CylinderFluxes fluxes;

    // Mean piston speed for Woschni: running average of |v| over roughly one revolution.
    const double alpha = std::min(1.0, dt / 0.02);
    m_meanPistonSpeed += alpha * (std::abs(inputs.pistonSpeed) - m_meanPistonSpeed);

    const double freshFuelFraction = 1.0 / (1.0 + p.combustion.airFuelRatio);
    const bool ignitable = m_state.fuelMass >= p.combustion.leanLimit * freshFuelFraction * m_state.mass;
    if (inputs.spark && ignitable && m_state.timeSinceSpark < 0.0)
    {
        m_state.timeSinceSpark = 0.0;
        m_fuelAtSpark = m_state.fuelMass;
        // Below ~10 rpm the engine is not turning; cap the burn time there.
        m_burnSeconds = p.combustion.durationDeg / std::max(std::abs(inputs.crankSpeed), 60.0);
    }

    // Heun (RK2) over substeps; the piston moves linearly within the step.
    const int substeps = 4;
    const double h = dt / substeps;
    const double volumeDot = p.geometry.PistonArea() * inputs.pistonSpeed;
    double x = inputs.pistonFromTdc - inputs.pistonSpeed * dt;  // position at the start of the step
    double mass = m_state.mass;
    double energy = mass * gas.InternalEnergy(m_state.temperature);
    double fuel = m_state.fuelMass;

    for (int s = 0; s < substeps; ++s)
    {
        double burnRate = 0.0;
        double xbNext = m_state.burnedFraction;
        if (m_state.timeSinceSpark >= 0.0)
        {
            xbNext = WiebeFraction(m_state.timeSinceSpark + h);
            burnRate = m_fuelAtSpark * (xbNext - m_state.burnedFraction) / h;
            burnRate = std::min(burnRate, std::max(fuel, 0.0) / h);
        }

        const double v0 = Volume(x), v1 = Volume(x + inputs.pistonSpeed * h);
        const double t0 = gas.TemperatureFromEnergy(energy / mass);
        const Rates k1 = Derivatives(mass, t0, fuel, v0, volumeDot, inputs, burnRate);
        const double m1 = mass + h * k1.massDot, e1 = energy + h * k1.energyDot, f1 = fuel + h * k1.fuelDot;
        const double t1 = gas.TemperatureFromEnergy(e1 / m1);
        const Rates k2 = Derivatives(m1, t1, f1, v1, volumeDot, inputs, burnRate);

        mass += 0.5 * h * (k1.massDot + k2.massDot);
        energy += 0.5 * h * (k1.energyDot + k2.energyDot);
        fuel = std::max(fuel + 0.5 * h * (k1.fuelDot + k2.fuelDot), 0.0);

        fluxes.work += 0.5 * h * (k1.workDot + k2.workDot);
        fluxes.heatReleased += 0.5 * h * (k1.heatReleaseDot + k2.heatReleaseDot);
        fluxes.heatLost += 0.5 * h * (k1.heatLossDot + k2.heatLossDot);
        fluxes.massIn += 0.5 * h * (k1.intakeDot + k2.intakeDot);
        fluxes.massOut += 0.5 * h * (k1.exhaustDot + k2.exhaustDot);

        x += inputs.pistonSpeed * h;
        if (m_state.timeSinceSpark >= 0.0)
        {
            m_state.timeSinceSpark += h;
            m_state.burnedFraction = xbNext;
            // Combustion over: ready for the next spark.
            if (m_state.timeSinceSpark >= m_burnSeconds)
            {
                m_state.timeSinceSpark = -1.0;
                m_state.burnedFraction = 0.0;
            }
        }
    }

    m_state.mass = mass;
    m_state.temperature = gas.TemperatureFromEnergy(energy / mass);
    m_state.volume = Volume(inputs.pistonFromTdc);
    m_state.pressure = mass * gas.R * m_state.temperature / m_state.volume;
    m_state.fuelMass = fuel;
    return fluxes;
}

}  // namespace aiad::sim::engine
