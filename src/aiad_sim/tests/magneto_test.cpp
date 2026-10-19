// M1.3 acceptance test: flywheel magneto field table (MFEM) and breaker-point ignition circuit.
#include "magneto/MagnetoField.h"
#include "magneto/MagnetoIgnition.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

using namespace aiad::sim::magneto;

namespace
{

bool Check(const char* what, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

// Sweep the magnet through the window at constant speed; return the secondary peak at the opening.
IgnitionStep SweepOnce(const std::vector<MagnetoSample>& table, IgnitionCircuit circuit, double rpm, double pressure)
{
    MagnetoIgnition ignition(table, circuit);
    const double degPerSecond = rpm * 6.0, stepDeg = 0.02, dt = stepDeg / degPerSecond;
    IgnitionStep result;
    for (double a = table.front().angleDeg; a <= table.back().angleDeg; a += stepDeg)
    {
        const IgnitionStep s = ignition.Step(dt, a, pressure);
        if (s.secondaryPeak > 0.0)
            result = s;
    }
    return result;
}

}  // namespace

int main()
{
    const MagnetoGeometry geometry;
    const MagnetoMeshOptions mesh;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<MagnetoSample> table = ComputeMagnetoTable(geometry, mesh, 45.0, 1.0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Magneto field table: %zu angles in %.1f s\n", table.size(), seconds);

    double phiMax = 0.0, asymmetry = 0.0, inductanceAsymmetry = 0.0, minInductance = 1e9;
    for (size_t i = 0; i < table.size(); ++i)
    {
        const MagnetoSample& s = table[i];
        const MagnetoSample& m = table[table.size() - 1 - i];
        phiMax = std::max(phiMax, std::abs(s.fluxPerTurn));
        asymmetry = std::max(asymmetry, std::abs(s.fluxPerTurn - m.fluxPerTurn));
        inductanceAsymmetry = std::max(inductanceAsymmetry, std::abs(s.inductancePerTurnSquared - m.inductancePerTurnSquared));
        minInductance = std::min(minInductance, s.inductancePerTurnSquared);
        if (static_cast<int>(std::lround(s.angleDeg)) % 5 == 0)
            std::printf("  angle %+5.1f deg: Phi = %+.4e Wb, L/N^2 = %.4e H\n", s.angleDeg, s.fluxPerTurn,
                        s.inductancePerTurnSquared);
    }
    const double faceFlux = geometry.magnetRemanence * geometry.assemblyDepth * geometry.stackDepth;
    std::printf("  max |Phi| %.4e Wb (magnet face bound %.4e Wb), mirror asymmetry %.2e\n", phiMax, faceFlux,
                asymmetry / phiMax);

    bool ok = Check("|Phi| below the magnet-face bound Br*A", phiMax > 0.0 && phiMax < faceFlux);
    ok &= Check("Phi(-a) = Phi(a) within 2% (mirror symmetry)", asymmetry < 0.02 * phiMax);
    ok &= Check("inductance positive and mirror-symmetric (2%)",
                minInductance > 0.0 && inductanceAsymmetry < 0.02 * table[table.size() / 2].inductancePerTurnSquared);

    // Ignition: choose the point opening angle that maximises the spark at cranking speed.
    const double crankingRpm = 600.0, compressionPressure = 1.2e6;
    IgnitionCircuit circuit;
    const double bestOpen = OptimalPointsOpening(table, circuit, crankingRpm);
    circuit.pointsOpenDeg = bestOpen;
    MagnetoIgnition probe(table, circuit);
    std::printf("Ignition: points open at %+.0f deg, L1 there %.1f mH\n", bestOpen, probe.PrimaryInductance(bestOpen) * 1e3);
    double minSparkRpm = -1.0;
    for (double rpm : {100.0, 200.0, 300.0, 600.0, 1000.0, 3000.0, 6000.0})
    {
        const IgnitionStep s = SweepOnce(table, circuit, rpm, compressionPressure);
        std::printf("  %5.0f rpm: secondary %.1f kV vs breakdown %.1f kV -> %s\n", rpm, s.secondaryPeak / 1e3,
                    s.breakdownVoltage / 1e3, s.spark ? "spark" : "no spark");
        if (s.spark && minSparkRpm < 0.0)
            minSparkRpm = rpm;
    }
    const IgnitionStep cranking = SweepOnce(table, circuit, crankingRpm, compressionPressure);
    ok &= Check("secondary 10-30 kV at 600 rpm cranking", cranking.secondaryPeak > 10e3 && cranking.secondaryPeak < 30e3);
    ok &= Check("sparks at cranking speed against 12 bar compression", cranking.spark);
    std::printf("  lowest tested speed that sparks: %.0f rpm\n", minSparkRpm);

    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
