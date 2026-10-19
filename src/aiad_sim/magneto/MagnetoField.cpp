#include "MagnetoField.h"

#include <mfem.hpp>

#include "common/EigenConfig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <numbers>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace aiad::sim::magneto
{

namespace
{

constexpr double kMu0 = 4.0e-7 * std::numbers::pi;
constexpr double kDeg = std::numbers::pi / 180.0;

enum class Material
{
    Air,
    Magnet,
    Shoe,
    Core
};

// Angle difference wrapped to (-180, 180] [deg].
double WrapDeg(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a - 180.0;
}

struct Layout
{
    const MagnetoGeometry& g;

    double LegInner() const { return g.flywheelRadius + g.airGap; }
    double YokeInner() const { return LegInner() + g.legLength; }
    double YokeOuter() const { return YokeInner() + g.yokeThickness; }

    // Material at (x, y) with the magnet assembly centred at 90 + angleDeg.
    Material At(double x, double y, double angleDeg) const
    {
        const double r = std::hypot(x, y);
        const double phi = std::atan2(y, x) / kDeg;
        const double w = g.segmentAngleDeg;

        if (r >= g.flywheelRadius - g.assemblyDepth && r <= g.flywheelRadius)
        {
            const double d = std::abs(WrapDeg(phi - (90.0 + angleDeg)));
            if (d <= w / 2.0)
                return Material::Magnet;
            if (d <= 1.5 * w)
                return Material::Shoe;
            return Material::Air;
        }
        if (r >= LegInner() && r <= YokeOuter())
        {
            for (double legCentre : {90.0 - w, 90.0 + w})
            {
                const double dphi = WrapDeg(phi - legCentre) * kDeg;
                if (std::cos(dphi) > 0.0 && std::abs(r * std::sin(dphi)) <= g.legWidth / 2.0)
                    return Material::Core;
            }
            if (r >= YokeInner() && std::abs(WrapDeg(phi - 90.0)) <= w + (g.legWidth / 2.0) / r / kDeg)
                return Material::Core;
        }
        return Material::Air;
    }

    // Coil sides: +1 inside the U window (under the yoke), -1 just outside the yoke, 0 elsewhere.
    int CoilSide(double x, double y) const
    {
        const double r = std::hypot(x, y);
        const double phi = std::atan2(y, x) / kDeg;
        const double clearance = 0.001;
        if (std::abs(WrapDeg(phi - 90.0)) > g.segmentAngleDeg - (g.legWidth / 2.0 + clearance) / r / kDeg)
            return 0;
        if (r >= YokeInner() - 0.012 && r <= YokeInner() - clearance)
            return +1;
        if (r >= YokeOuter() + clearance && r <= YokeOuter() + 0.012)
            return -1;
        return 0;
    }

    // Graded refinement: level 0-1 around the whole core, level 2 over the magnet/shoe band up to the
    // gap, level 3 over the air gap. The iron (high permeability) has smooth fields and stays coarse.
    bool Refine(int level, double x, double y) const
    {
        const double r = std::hypot(x, y);
        const double phi = std::atan2(y, x) / kDeg;
        if (std::abs(WrapDeg(phi - 90.0)) > 3.0 * g.segmentAngleDeg + 10.0)
            return false;
        switch (level)
        {
        case 0:
        case 1: return r >= g.flywheelRadius - g.assemblyDepth - 0.003 && r <= YokeOuter() + 0.015;
        case 2: return r >= g.flywheelRadius - g.assemblyDepth - 0.001 && r <= LegInner() + 0.003;
        case 3: return r >= g.flywheelRadius - 0.0015 && r <= LegInner() + 0.0015;
        default: return false;
        }
    }
};

}  // namespace

namespace
{

// The fixed part of the field problem (mesh, space, boundary, coil functionals and source) and the solve
// at one rotor angle. MFEM's assembly keeps scratch state in the mesh: each thread has its own.
struct FieldProblem
{
    const MagnetoGeometry& geometry;
    const Layout layout;
    mfem::Mesh mesh;
    mfem::H1_FECollection fec{1, 2};
    std::unique_ptr<mfem::FiniteElementSpace> fes;
    mfem::Array<int> essDofs;
    std::unique_ptr<mfem::LinearForm> inner, outer, coilRhs;
    double innerArea = 0.0, outerArea = 0.0;
    // High-order quadrature samples the moving material boundaries inside each element.
    const mfem::IntegrationRule& ir;

    FieldProblem(const MagnetoGeometry& g, const MagnetoMeshOptions& meshOptions, const mfem::IntegrationRule& rule)
        : geometry(g), layout{g}, ir(rule)
    {
        const double s = meshOptions.domainHalfSize;

        // Fixed mesh, locally refined (non-conforming) around the core and the air gap.
        mesh = mfem::Mesh::MakeCartesian2D(meshOptions.baseCells, meshOptions.baseCells,
                                                      mfem::Element::QUADRILATERAL, true, 2.0 * s, 2.0 * s);
        mesh.Transform([s](const mfem::Vector& in, mfem::Vector& out) {
            out.SetSize(2);
            out(0) = in(0) - s;
            out(1) = in(1) - s;
        });
        mesh.EnsureNCMesh();
        for (int level = 0; level < 4; ++level)
        {
            mfem::Array<int> marked;
            mfem::Vector centre;
            for (int e = 0; e < mesh.GetNE(); ++e)
            {
                mesh.GetElementCenter(e, centre);
                if (layout.Refine(level, centre(0), centre(1)))
                    marked.Append(e);
            }
            mesh.GeneralRefinement(marked, 1);
        }

        fes = std::make_unique<mfem::FiniteElementSpace>(&mesh, &fec);
        mfem::Array<int> essBdr(mesh.bdr_attributes.Max());
        essBdr = 1;
        fes->GetEssentialTrueDofs(essBdr, essDofs);

        // High-order quadrature samples the moving material boundaries inside each element.

        // Coil-side averaging functionals and areas (fixed stator).
        mfem::FunctionCoefficient innerSide([this](const mfem::Vector& p) { return layout.CoilSide(p(0), p(1)) > 0 ? 1.0 : 0.0; });
        mfem::FunctionCoefficient outerSide([this](const mfem::Vector& p) { return layout.CoilSide(p(0), p(1)) < 0 ? 1.0 : 0.0; });
        inner = std::make_unique<mfem::LinearForm>(fes.get());
        outer = std::make_unique<mfem::LinearForm>(fes.get());
        {
            auto* i1 = new mfem::DomainLFIntegrator(innerSide);
            i1->SetIntRule(&ir);
            inner->AddDomainIntegrator(i1);
            auto* i2 = new mfem::DomainLFIntegrator(outerSide);
            i2->SetIntRule(&ir);
            outer->AddDomainIntegrator(i2);
        }
        inner->Assemble();
        outer->Assemble();
        mfem::GridFunction one(fes.get());
        one = 1.0;
        innerArea = (*inner)(one);
        outerArea = (*outer)(one);
        if (innerArea <= 0.0 || outerArea <= 0.0)
            throw std::runtime_error("magneto: coil sides are not resolved by the mesh");

        // Unit ampere-turn coil current density (+ in the window, - outside the yoke).
        mfem::FunctionCoefficient coilCurrent([this](const mfem::Vector& p) {
            const int side = layout.CoilSide(p(0), p(1));
            return side > 0 ? 1.0 / innerArea : side < 0 ? -1.0 / outerArea : 0.0;
        });
        coilRhs = std::make_unique<mfem::LinearForm>(fes.get());
        {
            auto* integ = new mfem::DomainLFIntegrator(coilCurrent);
            integ->SetIntRule(&ir);
            coilRhs->AddDomainIntegrator(integ);
        }
        coilRhs->Assemble();
    }

    double FluxPerTurn(const mfem::GridFunction& a) const { return geometry.stackDepth * ((*inner)(a) / innerArea - (*outer)(a) / outerArea); }

    MagnetoSample Solve(double angle) const
    {
        const double magnetization = geometry.magnetRemanence / (kMu0 * geometry.magnetRecoilPermeability);
        mfem::FunctionCoefficient nu([&](const mfem::Vector& p) {
            switch (layout.At(p(0), p(1), angle))
            {
            case Material::Magnet: return 1.0 / (kMu0 * geometry.magnetRecoilPermeability);
            case Material::Shoe: return 1.0 / (kMu0 * geometry.shoeRelativePermeability);
            case Material::Core: return 1.0 / (kMu0 * geometry.coreRelativePermeability);
            default: return 1.0 / kMu0;
            }
        });
        // Tangential magnetization M (CCW); the weak-form source is (Q, grad v) with Q = (-M_y, M_x).
        mfem::VectorFunctionCoefficient q(2, [&](const mfem::Vector& p, mfem::Vector& out) {
            out.SetSize(2);
            out = 0.0;
            if (layout.At(p(0), p(1), angle) != Material::Magnet)
                return;
            const double phi = std::atan2(p(1), p(0));
            const double mx = -std::sin(phi) * magnetization, my = std::cos(phi) * magnetization;
            out(0) = -my;
            out(1) = mx;
        });

        mfem::BilinearForm a(fes.get());
        a.AddDomainIntegrator(new mfem::DiffusionIntegrator(nu, &ir));
        a.Assemble();
        mfem::LinearForm magnetRhs(fes.get());
        {
            auto* integ = new mfem::DomainLFGradIntegrator(q);
            integ->SetIntRule(&ir);
            magnetRhs.AddDomainIntegrator(integ);
        }
        magnetRhs.Assemble();

        mfem::GridFunction aMagnet(fes.get()), aCoil(fes.get());
        aMagnet = 0.0;
        aCoil = 0.0;
        mfem::OperatorPtr A;
        mfem::Vector X, B;
        a.FormLinearSystem(essDofs, aMagnet, magnetRhs, A, X, B);

        // Sparse Cholesky (Eigen), shared by the magnet and the coil right-hand sides.
        const mfem::SparseMatrix& S = *A.As<mfem::SparseMatrix>();
        const Eigen::Map<const Eigen::SparseMatrix<double, Eigen::RowMajor, int>> view(
            S.Height(), S.Width(), S.NumNonZeroElems(), S.GetI(), S.GetJ(), S.GetData());
        const Eigen::SparseMatrix<double> matrix = view;
        Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> llt(matrix);
        if (llt.info() != Eigen::Success)
            throw std::runtime_error("magneto: stiffness matrix is not positive definite");

        auto solve = [&](mfem::Vector& rhs, mfem::Vector& sol) {
            const Eigen::Map<const Eigen::VectorXd> b(rhs.GetData(), rhs.Size());
            const Eigen::VectorXd x = llt.solve(b);
            sol.SetSize(rhs.Size());
            for (int i = 0; i < rhs.Size(); ++i)
                sol(i) = x(i);
        };
        solve(B, X);
        a.RecoverFEMSolution(X, magnetRhs, aMagnet);

        mfem::Vector coil(*coilRhs);   // FormLinearSystem may change its right-hand side
        a.FormLinearSystem(essDofs, aCoil, coil, A, X, B);
        solve(B, X);
        a.RecoverFEMSolution(X, coil, aCoil);
        return {angle, FluxPerTurn(aMagnet), FluxPerTurn(aCoil)};
    }
};

}  // namespace

std::vector<MagnetoSample> ComputeMagnetoTable(const MagnetoGeometry& geometry, const MagnetoMeshOptions& meshOptions,
                                               double halfWindowDeg, double stepDeg, const std::atomic<bool>* cancel)
{
    // The angles are independent: every core solves its share, each with its own copy of the problem.
    const int count = static_cast<int>(std::floor(2.0 * halfWindowDeg / stepDeg + 1e-9)) + 1;
    const mfem::IntegrationRule& ir = mfem::IntRules.Get(mfem::Geometry::SQUARE, 10);   // built once, before the threads
    std::vector<MagnetoSample> table(static_cast<size_t>(count));
    std::atomic<int> next = 0;
    std::atomic<bool> failed = false;
    std::string error;
    std::mutex errorMutex;
    auto work = [&] {
        try
        {
            const FieldProblem problem(geometry, meshOptions, ir);
            for (int k = next++; k < count && !failed && !(cancel && *cancel); k = next++)
                table[size_t(k)] = problem.Solve(-halfWindowDeg + k * stepDeg);
        }
        catch (const std::exception& e)
        {
            std::lock_guard lock(errorMutex);
            error = e.what();
            failed = true;
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, count);
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t)
        pool.emplace_back(work);
    work();
    for (std::thread& t : pool)
        t.join();
    if (failed)
        throw std::runtime_error(error);
    if (cancel && *cancel)
        return {};
    return table;
}

std::vector<MagnetoSample> CachedMagnetoTable(const char* cachePath, const MagnetoGeometry& g, const MagnetoMeshOptions& m,
                                              double halfWindowDeg, double stepDeg, const std::atomic<bool>* cancel)
{
    // Exact inputs, printed with round-trip precision.
    char key[1024];
    std::snprintf(key, sizeof(key),
                  "magneto-v1 %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d %.17g %.17g",
                  g.flywheelRadius, g.assemblyDepth, g.segmentAngleDeg, g.airGap, g.legWidth, g.legLength,
                  g.yokeThickness, g.stackDepth, g.magnetRemanence, g.magnetRecoilPermeability,
                  g.coreRelativePermeability, g.shoeRelativePermeability, m.domainHalfSize, m.baseCells,
                  halfWindowDeg, stepDeg);
    {
        std::ifstream in(cachePath);
        std::string header;
        if (in && std::getline(in, header) && header == key)
        {
            std::vector<MagnetoSample> table;
            MagnetoSample s;
            while (in >> s.angleDeg >> s.fluxPerTurn >> s.inductancePerTurnSquared)
                table.push_back(s);
            if (!table.empty())
                return table;
        }
    }
    std::vector<MagnetoSample> table = ComputeMagnetoTable(g, m, halfWindowDeg, stepDeg, cancel);
    if (table.empty())
        return table;
    std::ofstream out(cachePath);
    out << key << '\n';
    char line[128];
    for (const MagnetoSample& s : table)
    {
        std::snprintf(line, sizeof(line), "%.17g %.17g %.17g\n", s.angleDeg, s.fluxPerTurn, s.inductancePerTurnSquared);
        out << line;
    }
    return table;
}

}  // namespace aiad::sim::magneto
