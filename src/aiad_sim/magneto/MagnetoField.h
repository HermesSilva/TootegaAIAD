// Flywheel magneto: 2D magnetostatics (MFEM) of a permanent magnet and two steel pole shoes on the
// rim of a non-magnetic flywheel passing a laminated U core with a coil on its yoke.
//
// Plane cross-section, out-of-plane vector potential A_z, linear materials:
//   int nu grad(A).grad(v) = int J v + int (M_x dv/dy - M_y dv/dx),  A = 0 on the far boundary.
// The rotor is not remeshed: the mesh is fixed and the material of every quadrature point is taken
// from the rotor geometry turned by the requested angle.
// Coil flux linkage per turn: Phi = Lz * (<A>_inner side - <A>_outer side).
#pragma once

#include <atomic>
#include <vector>

namespace aiad::sim::magneto
{

struct MagnetoGeometry
{
    // Flywheel (non-magnetic) and the magnet assembly on its rim.
    double flywheelRadius = 0.075;       // [m]
    double assemblyDepth = 0.012;        // radial thickness of magnet and shoes [m]
    double segmentAngleDeg = 10.0;       // angular width of the magnet and of each shoe [deg]
    // Stator U core: two radial legs centred at 90 deg +/- segmentAngleDeg, joined by a yoke.
    double airGap = 0.0005;              // [m]
    double legWidth = 0.008;             // tangential width of each leg [m]
    double legLength = 0.035;            // from the air gap to the yoke [m]
    double yokeThickness = 0.010;        // [m]
    double stackDepth = 0.025;           // out-of-plane length Lz [m]
    // Materials.
    double magnetRemanence = 0.4;        // Br [T] (ferrite, as in small-engine flywheel magnetos; NdFeB ~1.2)
    double magnetRecoilPermeability = 1.1;
    double coreRelativePermeability = 2000.0;
    double shoeRelativePermeability = 1000.0;
};

struct MagnetoMeshOptions
{
    double domainHalfSize = 0.15;        // square domain [-s, s]^2 [m]
    int baseCells = 120;                 // per side; 4 graded local refinements reach base/16 in the air gap
};

struct MagnetoSample
{
    double angleDeg = 0.0;               // assembly centre relative to the core axis [deg]
    double fluxPerTurn = 0.0;            // Phi [Wb]
    double inductancePerTurnSquared = 0.0;  // L / N^2 [H] (coil alone, magnet removed)
};

// Computes Phi(angle) and L/N^2(angle) for angles in [-halfWindowDeg, +halfWindowDeg].
// `cancel` (optional) stops it between angles: the table is then incomplete (empty) and not cached.
std::vector<MagnetoSample> ComputeMagnetoTable(const MagnetoGeometry& geometry, const MagnetoMeshOptions& mesh,
                                               double halfWindowDeg, double stepDeg, const std::atomic<bool>* cancel = nullptr);

// Same as ComputeMagnetoTable, reusing cachePath when it was computed with exactly the same inputs
// (all parameters are stored in the file and compared); otherwise computes and rewrites it.
std::vector<MagnetoSample> CachedMagnetoTable(const char* cachePath, const MagnetoGeometry& geometry,
                                              const MagnetoMeshOptions& mesh, double halfWindowDeg, double stepDeg,
                                              const std::atomic<bool>* cancel = nullptr);

}  // namespace aiad::sim::magneto
