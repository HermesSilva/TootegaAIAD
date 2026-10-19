// M1.2 acceptance test for the 0D cylinder model, driven by closed-form crank-slider kinematics.
//   1. Ideal Otto cycle (valves shut, adiabatic, near-instant combustion at TDC): efficiency must
//      match 1 - r^(1 - gamma) and the energy balance must close.
//   2. Real four-stroke cycle at 3000 rpm (cam-driven valves, spark advance, Woschni): the cycle
//      must converge and give plausible IMEP, peak pressure, volumetric and indicated efficiency.
#include "engine/EngineCylinder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

using namespace aiad::sim::engine;

namespace
{

constexpr double kPi = std::numbers::pi;
constexpr double kRodLength = 0.100;

// Piston distance below TDC for crank angle theta (0 = TDC) [m].
double PistonFromTdc(const CylinderGeometry& g, double theta)
{
    const double r = g.stroke / 2.0, l = kRodLength, s = std::sin(theta);
    return r + l - (r * std::cos(theta) + std::sqrt(l * l - r * r * s * s));
}

double PistonSpeed(const CylinderGeometry& g, double theta, double omega)
{
    const double r = g.stroke / 2.0, l = kRodLength, s = std::sin(theta), c = std::cos(theta);
    return omega * r * s * (1.0 + r * c / std::sqrt(l * l - r * r * s * s));
}

// Valve lift for a cam event between open and close crank angles (degrees, 720-periodic).
double Lift(double thetaDeg, double openDeg, double closeDeg, double maxLift)
{
    double t = std::fmod(thetaDeg - openDeg + 720.0, 720.0);
    const double duration = std::fmod(closeDeg - openDeg + 720.0, 720.0);
    if (t >= duration)
        return 0.0;
    const double s = std::sin(kPi * t / duration);
    return maxLift * s * s;
}

bool Check(const char* what, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

bool IdealOttoCycle()
{
    std::printf("Ideal Otto cycle\n");
    CylinderParameters p;
    p.heat.enabled = false;
    p.gas.gamma0 = 1.35;  // calorically perfect gas: the closed-form efficiency assumes constant gamma
    p.gas.gammaSlope = 0.0;
    p.combustion.durationDeg = 0.36;  // 20 us at 3000 rpm: near-instant
    p.combustion.combustionEfficiency = 1.0;
    EngineCylinder cyl(p);

    const double omega = 2.0 * kPi * 50.0, dt = 1.0e-6;
    const CylinderGeometry& g = p.geometry;
    cyl.Reset(PistonFromTdc(g, kPi));  // start at BDC
    const double fuel = cyl.State().fuelMass;
    const double u0 = cyl.State().mass * p.gas.InternalEnergy(cyl.State().temperature);

    double work = 0.0, released = 0.0, peak = 0.0;
    bool fired = false;
    const int steps = static_cast<int>(std::round(2.0 * kPi / omega / dt));  // BDC -> TDC -> BDC
    for (int i = 1; i <= steps; ++i)
    {
        const double theta = kPi + omega * dt * i;
        CylinderInputs in;
        in.pistonFromTdc = PistonFromTdc(g, theta);
        in.pistonSpeed = PistonSpeed(g, theta, omega);
        in.crankSpeed = omega * 180.0 / kPi;
        in.spark = !fired && theta >= 2.0 * kPi;
        fired = fired || in.spark;
        const CylinderFluxes f = cyl.Step(dt, in);
        work += f.work;
        released += f.heatReleased;
        peak = std::max(peak, cyl.State().pressure);
    }
    const double u1 = cyl.State().mass * p.gas.InternalEnergy(cyl.State().temperature);
    const double eta = -work / released;
    const double etaIdeal = 1.0 - std::pow(g.compressionRatio, 1.0 - p.gas.gamma0);
    std::printf("  fuel %.3e kg, heat released %.2f J (%.4f of available), peak %.1f bar\n", fuel, released,
                released / (fuel * p.combustion.fuelLowerHeatingValue), peak / 1e5);
    std::printf("  efficiency %.5f, ideal %.5f, rel. error %.2e\n", eta, etaIdeal, std::abs(eta - etaIdeal) / etaIdeal);
    const double closure = std::abs((u1 - u0) - (work + released)) / released;
    std::printf("  energy balance residual %.2e (of heat released)\n", closure);

    bool ok = Check("efficiency within 1% of 1 - r^(1-gamma)", std::abs(eta - etaIdeal) / etaIdeal < 0.01);
    ok &= Check("all fuel burned (>= 99%)", released >= 0.99 * fuel * p.combustion.fuelLowerHeatingValue);
    ok &= Check("energy balance closes (< 1e-6)", closure < 1e-6);
    return ok;
}

bool RealFourStrokeCycle()
{
    std::printf("Real four-stroke cycle, 3000 rpm, wide-open throttle\n");
    CylinderParameters p;
    EngineCylinder cyl(p);
    const CylinderGeometry& g = p.geometry;

    const double rpm = 3000.0, omega = rpm / 60.0 * 2.0 * kPi, dt = 2.0e-6;
    const double ivo = 350.0, ivc = 580.0, evo = 140.0, evc = 370.0, spark = 695.0, maxLift = 0.008;
    const int stepsPerCycle = static_cast<int>(std::round(4.0 * kPi / omega / dt));
    const double vd = g.DisplacedVolume();
    const double rhoIntake = p.intake.pressure / (p.gas.R * p.intake.temperature);

    cyl.Reset(PistonFromTdc(g, 0.0));
    double imep = 0.0, imepPrev = 0.0, peak = 0.0, released = 0.0, inducted = 0.0, work = 0.0;
    const int cycles = 6;
    for (int c = 0; c < cycles; ++c)
    {
        work = released = inducted = peak = 0.0;
        for (int i = 1; i <= stepsPerCycle; ++i)
        {
            const double thetaDeg = std::fmod(omega * dt * (static_cast<double>(c) * stepsPerCycle + i) * 180.0 / kPi, 720.0);
            const double theta = thetaDeg * kPi / 180.0;
            const double prevDeg = std::fmod(thetaDeg - omega * dt * 180.0 / kPi + 720.0, 720.0);
            CylinderInputs in;
            in.pistonFromTdc = PistonFromTdc(g, theta);
            in.pistonSpeed = PistonSpeed(g, theta, omega);
            in.crankSpeed = omega * 180.0 / kPi;
            in.intakeLift = Lift(thetaDeg, ivo, ivc, maxLift);
            in.exhaustLift = Lift(thetaDeg, evo, evc, maxLift);
            in.spark = prevDeg < spark && thetaDeg >= spark;
            const CylinderFluxes f = cyl.Step(dt, in);
            work += f.work;
            released += f.heatReleased;
            inducted += f.massIn;
            peak = std::max(peak, cyl.State().pressure);
        }
        imepPrev = imep;
        imep = -work / vd;
        std::printf("  cycle %d: IMEP %.2f bar, peak %.1f bar, inducted %.1f mg, heat %.1f J\n", c + 1, imep / 1e5,
                    peak / 1e5, inducted * 1e6, released);
    }
    const double volumetricEfficiency = inducted / (rhoIntake * vd);
    const double indicatedEfficiency = -work / (inducted / (1.0 + p.combustion.airFuelRatio) * p.combustion.fuelLowerHeatingValue);
    const double power = -work * rpm / 120.0;
    std::printf("  volumetric efficiency %.3f, indicated efficiency %.3f, indicated power %.2f kW (%.0f cc)\n",
                volumetricEfficiency, indicatedEfficiency, power / 1e3, vd * 1e6);

    bool ok = Check("cycle converged (IMEP change < 1%)", std::abs(imep - imepPrev) / imep < 0.01);
    ok &= Check("IMEP 7-14 bar", imep > 7e5 && imep < 14e5);
    ok &= Check("peak pressure 30-90 bar", peak > 30e5 && peak < 90e5);
    ok &= Check("volumetric efficiency 0.70-1.05", volumetricEfficiency > 0.70 && volumetricEfficiency < 1.05);
    ok &= Check("indicated efficiency 0.28-0.45", indicatedEfficiency > 0.28 && indicatedEfficiency < 0.45);
    return ok;
}

}  // namespace

int main()
{
    bool ok = IdealOttoCycle();
    ok &= RealFourStrokeCycle();
    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
