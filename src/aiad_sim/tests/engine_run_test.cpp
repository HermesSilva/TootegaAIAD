// M1.4 acceptance test: the coupled single-cylinder engine starts on the starter motor and keeps
// running on its own combustion; without spark it stops once the starter is released.
#include "engine/EngineSimulation.h"
#include "magneto/MagnetoField.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

using namespace aiad::sim;

namespace
{

bool Check(const char* what, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

struct RunSummary
{
    double minRpmAfterStarter = 1e9;
    double meanRpm1 = 0.0, meanRpm2 = 0.0;   // [3.0, 3.5) s and [3.5, 4.0) s
    double finalRpm = 0.0;
    int combustionsLastSecond = 0;
    int sparks = 0;
    double peakPressure = 0.0;
    double meanGasTorqueLastSecond = 0.0;
};

RunSummary Run(const engine::EngineDesign& design, const std::vector<magneto::MagnetoSample>& table, const char* csv)
{
    engine::EngineSimulation sim(design, table);
    std::ofstream out(csv);
    out << "time,crank_deg,rpm,pressure_bar,volume_cc,gas_torque,spark,secondary_kv\n";
    const double dt = 2.0e-5, tEnd = 4.0;
    RunSummary s;
    int n1 = 0, n2 = 0, nLast = 0;
    bool wasBurning = false;
    for (int i = 0; sim.Time() < tEnd - dt / 2; ++i)
    {
        const engine::EngineSample e = sim.Step(dt);
        if (i % 10 == 0 || e.spark)
            out << e.time << ',' << e.crankDeg << ',' << e.rpm << ',' << e.pressure / 1e5 << ',' << e.volume * 1e6 << ','
                << e.gasTorque << ',' << (e.spark ? 1 : 0) << ',' << e.secondaryPeak / 1e3 << '\n';
        s.sparks += e.spark ? 1 : 0;
        s.peakPressure = std::max(s.peakPressure, e.pressure);
        if (e.time > design.starterDuration + 0.05)
            s.minRpmAfterStarter = std::min(s.minRpmAfterStarter, e.rpm);
        if (e.time >= 3.0 && e.time < 3.5)
            s.meanRpm1 += e.rpm, ++n1;
        if (e.time >= 3.5)
            s.meanRpm2 += e.rpm, ++n2;
        if (e.time >= 3.0)
        {
            s.combustionsLastSecond += (e.combustion && !wasBurning) ? 1 : 0;
            s.meanGasTorqueLastSecond += e.gasTorque, ++nLast;
        }
        wasBurning = e.combustion;
        s.finalRpm = e.rpm;
    }
    s.meanRpm1 /= std::max(n1, 1);
    s.meanRpm2 /= std::max(n2, 1);
    s.meanGasTorqueLastSecond /= std::max(nLast, 1);
    return s;
}

}  // namespace

int main()
{
    const std::vector<magneto::MagnetoSample> table = magneto::CachedMagnetoTable("magneto_table.txt", {}, {}, 45.0, 1.0);

    engine::EngineDesign design;
    design.ignition.pointsOpenDeg = magneto::OptimalPointsOpening(table, design.ignition, 600.0);
    // Spark at 25 deg before the firing TDC (cycle angle 695 = 335 mod 360).
    design.magnetMountDeg = 335.0 - design.ignition.pointsOpenDeg;
    std::printf("Points open at magnet angle %+.0f deg, magnet mounted at crank %.0f deg\n",
                design.ignition.pointsOpenDeg, design.magnetMountDeg);

    std::printf("Run with ignition\n");
    const RunSummary on = Run(design, table, "engine_run.csv");
    const double omega = on.meanRpm2 * 2.0 * 3.14159265358979 / 60.0;
    std::printf("  min rpm after starter %.0f, mean rpm %.0f -> %.0f, sparks %d, combustions in last s %d\n",
                on.minRpmAfterStarter, on.meanRpm1, on.meanRpm2, on.sparks, on.combustionsLastSecond);
    std::printf("  peak pressure %.1f bar, mean gas torque %.2f N m, brake power %.2f kW (load)\n", on.peakPressure / 1e5,
                on.meanGasTorqueLastSecond, design.loadCoefficient * omega * omega * omega / 1e3);

    bool ok = Check("keeps running after the starter (> 1000 rpm)", on.minRpmAfterStarter > 1000.0);
    ok &= Check("steady state reached (mean rpm change < 3%)", std::abs(on.meanRpm2 - on.meanRpm1) / on.meanRpm2 < 0.03);
    ok &= Check("speed 1500-7000 rpm", on.meanRpm2 > 1500.0 && on.meanRpm2 < 7000.0);
    const double expectedFirings = on.meanRpm2 / 120.0;  // one combustion every two revolutions
    ok &= Check("one combustion per two revolutions", std::abs(on.combustionsLastSecond - expectedFirings) <= 2.0);

    std::printf("Run without ignition (control)\n");
    engine::EngineDesign dead = design;
    dead.ignition.secondaryTurns = 1;  // no usable spark
    const RunSummary off = Run(dead, table, "engine_run_no_spark.csv");
    std::printf("  final rpm %.0f\n", off.finalRpm);
    ok &= Check("without spark the engine stops after the starter (< 50 rpm)", std::abs(off.finalRpm) < 50.0);

    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
