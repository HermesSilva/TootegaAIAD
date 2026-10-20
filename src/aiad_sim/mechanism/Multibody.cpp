#include "Multibody.h"

#include <chrono/functions/ChFunctionConst.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkMate.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChShaft.h>
#include <chrono/physics/ChShaftBodyConstraint.h>
#include <chrono/physics/ChShaftsGear.h>
#include <chrono/physics/ChShaftsPlanetary.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChDirectSolverLS.h>
#include <chrono/solver/ChKRMBlock.h>
#include <chrono/solver/ChSystemDescriptor.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <numbers>
#include <set>

namespace aiad::sim::mb
{

using chrono::ChVector3d;
using chrono::ChQuaterniond;

namespace
{

constexpr double kDampingRatio = 0.3;   // contacts and stops
// Touches (soft contact, as Box2D's soft step): the spring at this fraction of the step rate, this damping ratio, on
// the reduced mass at the contact; machines with touches step at most kTouchStep (an escapement's events).
constexpr double kTouchRate = 0.25, kTouchDamping = 10.0, kTouchStep = 5e-5;


// Penalty stiffness that makes `mass` bounce at kAutoContactFrequency.
double AutoStiffness(double mass)
{
    const double w = 2.0 * std::numbers::pi * kAutoContactFrequency;
    return mass < 1e29 ? mass * w * w : 1e300;
}

// The system descriptor, with the stiffness/damping blocks it collects in view (the direct assembly writes them).
class Descriptor : public chrono::ChSystemDescriptor
{
public:
    const std::vector<chrono::ChKRMBlock*>& Blocks() const { return m_KRMblocks; }
};

// A contact's stiffness and damping between two bodies in the step's matrix (implicit): -dQ/dx and -dQ/dv of the
// contacts of this step, k J^T J and c J^T J, J taking the bodies' speeds to the closing speed. Stable at any step,
// however stiff the contact and light the parts (a watch's fork between its pin and its escape wheel); the force
// itself (k depth) is applied as a load. Touches and joint stops.
class PairStiffness : public chrono::ChPhysicsItem
{
public:
    using Jacobian = Eigen::Matrix<double, 12, 1>;   // body0 (speed, angular speed in its axes), then body1
    PairStiffness(chrono::ChVariables* a, chrono::ChVariables* b) : m_block(a, b)
    {
        Clear();
        m_block.GetMatrix().setZero(12, 12);
    }
    ChPhysicsItem* Clone() const override { return new PairStiffness(*this); }
    void InjectKRMMatrices(chrono::ChSystemDescriptor& descriptor) override { descriptor.InsertKRMBlock(&m_block); }
    void LoadKRMMatrices(double Kfactor, double Rfactor, double) override { m_block.GetMatrix() = Kfactor * m_K + Rfactor * m_R; }
    void Clear()
    {
        m_K.setZero();
        m_R.setZero();
    }
    void Add(const Jacobian& j, double stiffness, double damping)
    {
        const Eigen::Matrix<double, 12, 12> jj = j * j.transpose();
        m_K += stiffness * jj;
        m_R += damping * jj;
    }

private:
    chrono::ChKRMBlock m_block;
    Eigen::Matrix<double, 12, 12> m_K, m_R;
};

// The sparse LU of the step's system. Its pattern never changes and its values change a little per step, so
// (as KLU does for circuits) Eigen's SparseLU chooses the ordering and the pivots once, and the next steps
// refactor numerically with the same pivots on a structure computed once: no ordering, no symbolic work, no
// allocation (Eigen's supernodal LU spends most of its time on those for a system this small). Whenever a pivot
// gets small in its column, Eigen pivots afresh. (Chrono's implicit Euler also asks for a new ordering every
// step even with the pattern locked; it is redone here only when the pattern really changes.)
class SolverLU : public chrono::ChDirectSolverLS
{
public:
    Type GetType() const override { return Type::CUSTOM; }

    // Chrono writes every entry of the system matrix through a search in its row, every step (a fifth of the
    // step). The matrix is written here straight into its slots, found once; the first assembly is checked
    // against Chrono's, entry by entry, and any difference keeps Chrono's way.
    bool Setup(chrono::ChSystemDescriptor& sysd, bool analyze) override
    {
        if (m_direct && Assemble(sysd))
            return FactorizeMatrix(analyze);
        const bool ok = ChDirectSolverLS::Setup(sysd, analyze);
        if (ok && !m_checked)
        {
            m_checked = true;
            const std::vector<double> chrono(m_mat.valuePtr(), m_mat.valuePtr() + m_mat.nonZeros());
            m_direct = Slots(sysd) && Assemble(sysd) && std::equal(chrono.begin(), chrono.end(), m_mat.valuePtr());
            std::copy(chrono.begin(), chrono.end(), m_mat.valuePtr());
        }
        return ok;
    }

private:
    static constexpr double kPivotTolerance = 0.1;    // smallest |pivot| / largest |entry| below it
    // Largest backward error accepted for a solution with the kept pivots. Measured on the engines: with Eigen's
    // pivots and with the kept ones alike, at most 3e-17; with pivots accepted down to 1e-14 of their column,
    // up to 2e-13.
    static constexpr double kBackwardError = 1e-14;

    int Slot(int row, int col) const
    {
        const int* begin = m_mat.innerIndexPtr() + m_mat.outerIndexPtr()[row];
        const int* end = m_mat.innerIndexPtr() + m_mat.outerIndexPtr()[row + 1];
        const int* at = std::lower_bound(begin, end, col);
        return at != end && *at == col ? int(at - m_mat.innerIndexPtr()) : -1;
    }

    // The slot of every entry the masses, the constraint rows (and their transposes) and the compliances write.
    bool Slots(chrono::ChSystemDescriptor& sysd)
    {
        m_variables = int(sysd.CountActiveVariables());
        m_constraints = int(sysd.CountActiveConstraints());
        m_massSlots.clear();
        for (const chrono::ChVariables* v : sysd.GetVariables())
            if (v->IsActive())
                for (unsigned r = 0; r < v->GetDOF(); ++r)
                    for (unsigned c = 0; c < v->GetDOF(); ++c)
                        m_massSlots.push_back(Slot(int(v->GetOffset() + r), int(v->GetOffset() + c)));
        m_rowSlots.assign(1, 0);
        m_rowEntries.clear();
        m_complianceSlots.clear();
        const int n = m_variables;
        int row = 0;
        for (const chrono::ChConstraint* c : sysd.GetConstraints())
        {
            if (!c->IsActive())
                continue;
            for (int p = m_mat.outerIndexPtr()[n + row]; p < m_mat.outerIndexPtr()[n + row + 1]; ++p)
                if (const int col = m_mat.innerIndexPtr()[p]; col < n)
                    m_rowEntries.push_back({col, p, Slot(col, n + row)});
            m_rowSlots.push_back(int(m_rowEntries.size()));
            m_complianceSlots.push_back(Slot(n + row, n + row));
            ++row;
        }
        // Stiffness/damping blocks (implicit touches), variable pair by pair as Chrono pastes them.
        m_blockSlots.clear();
        const auto* descriptor = dynamic_cast<const Descriptor*>(&sysd);
        m_blocks = descriptor ? descriptor->Blocks().size() : 0;
        for (size_t b = 0; b < m_blocks; ++b)
        {
            const chrono::ChKRMBlock* block = descriptor->Blocks()[b];
            for (unsigned iv = 0; iv < block->GetNumVariables(); ++iv)
                for (unsigned jv = 0; jv < block->GetNumVariables(); ++jv)
                {
                    const chrono::ChVariables* a = block->GetVariable(iv);
                    const chrono::ChVariables* c = block->GetVariable(jv);
                    if (a->IsActive() && c->IsActive())
                        for (unsigned r = 0; r < a->GetDOF(); ++r)
                            for (unsigned k = 0; k < c->GetDOF(); ++k)
                                m_blockSlots.push_back(Slot(int(a->GetOffset() + r), int(c->GetOffset() + k)));
                }
        }
        m_unit.setZero(n);
        m_product.setZero(n);
        return std::all_of(m_rowEntries.begin(), m_rowEntries.end(), [](const RowEntry& e) { return e.transposed >= 0; });
    }

    // [ c_a M  Cq^T ; Cq  -E ] scaled as Chrono does; false when an entry has no slot (then Chrono assembles).
    bool Assemble(chrono::ChSystemDescriptor& sysd)
    {
        if (int(sysd.CountActiveVariables()) != m_variables || int(sysd.CountActiveConstraints()) != m_constraints)
            return false;
        double* values = m_mat.valuePtr();
        std::fill(values, values + m_mat.nonZeros(), 0.0);
        const double scale = conditioning_factor, ca = scale * sysd.GetMassFactor();
        size_t m = 0;
        for (const chrono::ChVariables* v : sysd.GetVariables())
        {
            if (!v->IsActive())
                continue;
            const unsigned offset = v->GetOffset(), dof = v->GetDOF();
            for (unsigned c = 0; c < dof; ++c)
            {
                m_unit[offset + c] = 1.0;
                v->AddMassTimesVectorInto(m_product, m_unit, ca);
                m_unit[offset + c] = 0.0;
                for (unsigned r = 0; r < dof; ++r)
                {
                    const double value = m_product[offset + r];
                    m_product[offset + r] = 0.0;
                    const int slot = m_massSlots[m + r * dof + c];
                    if (slot >= 0)
                        values[slot] = value;
                    else if (value != 0.0)
                        return false;
                }
            }
            m += size_t(dof) * dof;
        }
        const auto* descriptor = dynamic_cast<const Descriptor*>(&sysd);
        if ((descriptor ? descriptor->Blocks().size() : 0) != m_blocks)
            return false;
        size_t s = 0;
        for (size_t b = 0; b < m_blocks; ++b)
        {
            const chrono::ChKRMBlock* block = descriptor->Blocks()[b];
            const auto& matrix = const_cast<chrono::ChKRMBlock*>(block)->GetMatrix();
            unsigned io = 0;
            for (unsigned iv = 0; iv < block->GetNumVariables(); ++iv)
            {
                const chrono::ChVariables* a = block->GetVariable(iv);
                unsigned jo = 0;
                for (unsigned jv = 0; jv < block->GetNumVariables(); ++jv)
                {
                    const chrono::ChVariables* c = block->GetVariable(jv);
                    if (a->IsActive() && c->IsActive())
                        for (unsigned r = 0; r < a->GetDOF(); ++r)
                            for (unsigned k = 0; k < c->GetDOF(); ++k)
                            {
                                const double value = scale * matrix(io + r, jo + k);
                                const int slot = m_blockSlots[s++];
                                if (slot >= 0)
                                    values[slot] += value;
                                else if (value != 0.0)
                                    return false;
                            }
                    jo += c->GetDOF();
                }
                io += a->GetDOF();
            }
        }
        int row = 0;
        for (const chrono::ChConstraint* c : sysd.GetConstraints())
        {
            if (!c->IsActive())
                continue;
            c->AddJacobianTransposedTimesScalarInto(m_product, 1.0);
            for (int e = m_rowSlots[row]; e < m_rowSlots[row + 1]; ++e)
            {
                const RowEntry& entry = m_rowEntries[e];
                values[entry.slot] = values[entry.transposed] = m_product[entry.col];
                m_product[entry.col] = 0.0;
            }
            if (const double compliance = -c->GetComplianceTerm() / scale; m_complianceSlots[row] >= 0)
                values[m_complianceSlots[row]] = compliance;
            else if (compliance != 0.0)
                return false;
            ++row;
        }
        return true;
    }

    bool FactorizeMatrix(bool) override
    {
        const int* outer = m_mat.outerIndexPtr();
        const int* inner = m_mat.innerIndexPtr();
        if (m_outer.size() != size_t(m_mat.outerSize()) + 1 || !std::equal(m_outer.begin(), m_outer.end(), outer) ||
            m_inner.size() != size_t(m_mat.nonZeros()) || !std::equal(m_inner.begin(), m_inner.end(), inner))
        {
            m_outer.assign(outer, outer + m_mat.outerSize() + 1);
            m_inner.assign(inner, inner + m_mat.nonZeros());
            m_engine.analyzePattern(m_mat);
            m_symbolic = false;
        }
        if (m_symbolic && Refactor())
            return true;
        m_engine.factorize(m_mat);
        if (m_engine.info() != Eigen::Success)
            return false;
        m_symbolic = Symbolic() && Refactor();
        return true;
    }

    bool SolveSystem() override
    {
        if (!m_symbolic)
        {
            m_sol = m_engine.solve(m_rhs);
            return m_engine.info() == Eigen::Success;
        }
        const int n = int(m_rowOf.size());
        for (int i = 0; i < n; ++i)
            m_z[m_rowOf[i]] = m_rhs[i];
        for (int k = 0; k < n; ++k)
            if (const double zk = m_z[k]; zk != 0.0)
                for (int q = m_Lp[k]; q < m_Lp[k + 1]; ++q)
                    m_z[m_Li[q]] -= m_Lx[q] * zk;
        for (int j = n - 1; j >= 0; --j)
        {
            const double zj = m_z[j] /= m_pivot[j];
            if (zj != 0.0)
                for (int p = m_Up[j]; p < m_Up[j + 1]; ++p)
                    m_z[m_Ui[p]] -= m_Ux[p] * zj;
        }
        m_sol.resize(n);
        for (int i = 0; i < n; ++i)
            m_sol[i] = m_z[m_colOf[i]];
        if (BackwardError() <= kBackwardError)
            return true;
        // The kept pivots lost accuracy: Eigen pivots afresh (next steps refactor with its new pivots), and this
        // step is solved again.
        m_engine.factorize(m_mat);
        if (m_engine.info() != Eigen::Success)
            return false;
        m_symbolic = false;
        m_sol = m_engine.solve(m_rhs);
        return m_engine.info() == Eigen::Success;
    }

    // Normwise backward error of the solution (Rigal-Gaches): the smallest relative change of A and b that makes
    // it exact, |r| / (|A| |x| + |b|) in the infinity norm. (The componentwise one means nothing here: rows whose
    // entries are all rounding noise, ~1e-50, fail it for any solver.)
    double BackwardError() const
    {
        const int n = int(m_mat.rows());
        const int* outer = m_mat.outerIndexPtr();
        const int* inner = m_mat.innerIndexPtr();
        const double* values = m_mat.valuePtr();
        double residual = 0.0, matrix = 0.0, solution = 0.0, rhs = 0.0;
        for (int i = 0; i < n; ++i)
        {
            double r = m_rhs[i], row = 0.0;
            for (int p = outer[i]; p < outer[i + 1]; ++p)
            {
                r -= values[p] * m_sol[inner[p]];
                row += std::abs(values[p]);
            }
            residual = std::max(residual, std::abs(r));
            matrix = std::max(matrix, row);
            solution = std::max(solution, std::abs(m_sol[i]));
            rhs = std::max(rhs, std::abs(m_rhs[i]));
        }
        const double scale = matrix * solution + rhs;
        return scale > 0.0 ? residual / scale : residual;
    }

    // Eigen factors Pr A Pc^-1 = L U: B = Pr A Pc^-1 has an LU without pivoting. The pattern of every column of
    // L and U, from the pattern of B (left-looking: column j reaches the columns of L its U entries point to).
    bool Symbolic()
    {
        const int n = int(m_mat.rows());
        const auto& pr = m_engine.rowsPermutation().indices();
        const auto& pc = m_engine.colsPermutation().indices();
        m_rowOf.assign(pr.data(), pr.data() + n);
        m_colOf.assign(pc.data(), pc.data() + n);
        std::vector<std::vector<std::pair<int, int>>> columns(static_cast<size_t>(n));   // per column of B: (row, value index)
        for (int r = 0; r < n; ++r)
            for (int p = m_outer[size_t(r)]; p < m_outer[size_t(r) + 1]; ++p)
                columns[size_t(m_colOf[size_t(m_inner[size_t(p)])])].push_back({m_rowOf[size_t(r)], p});
        m_Bp.assign(1, 0);
        m_Bi.clear();
        m_Bv.clear();
        m_Lp.assign(1, 0);
        m_Li.clear();
        m_Up.assign(1, 0);
        m_Ui.clear();
        std::vector<char> mark(static_cast<size_t>(n), 0);
        for (int j = 0; j < n; ++j)
        {
            for (const auto& [row, value] : columns[size_t(j)])
            {
                m_Bi.push_back(row);
                m_Bv.push_back(value);
                mark[size_t(row)] = 1;
            }
            m_Bp.push_back(int(m_Bi.size()));
            for (int k = 0; k < j; ++k)
                if (mark[size_t(k)])
                {
                    m_Ui.push_back(k);
                    for (int q = m_Lp[size_t(k)]; q < m_Lp[size_t(k) + 1]; ++q)
                        mark[size_t(m_Li[size_t(q)])] = 1;
                }
            m_Up.push_back(int(m_Ui.size()));
            if (!mark[size_t(j)])
                return false;   // structurally zero pivot
            for (int i = j + 1; i < n; ++i)
                if (mark[size_t(i)])
                    m_Li.push_back(i);
            m_Lp.push_back(int(m_Li.size()));
            std::fill(mark.begin(), mark.end(), 0);
        }
        m_Lx.resize(m_Li.size());
        m_Ux.resize(m_Ui.size());
        m_pivot.resize(size_t(n));
        m_x.assign(size_t(n), 0.0);
        m_z.assign(size_t(n), 0.0);
        return true;
    }

    // Numeric LU of B with the kept pivots; false when one of them is too small in its column (then Eigen pivots).
    bool Refactor()
    {
        const int n = int(m_pivot.size());
        const double* values = m_mat.valuePtr();
        double* x = m_x.data();
        for (int j = 0; j < n; ++j)
        {
            for (int p = m_Bp[j]; p < m_Bp[j + 1]; ++p)
                x[m_Bi[p]] = values[m_Bv[p]];
            for (int p = m_Up[j]; p < m_Up[j + 1]; ++p)
            {
                const int k = m_Ui[p];
                const double xk = x[k];
                m_Ux[p] = xk;
                x[k] = 0.0;
                for (int q = m_Lp[k]; q < m_Lp[k + 1]; ++q)
                    x[m_Li[q]] -= m_Lx[q] * xk;
            }
            const double pivot = x[j];
            x[j] = 0.0;
            double largest = 0.0;
            for (int q = m_Lp[j]; q < m_Lp[j + 1]; ++q)
                largest = std::max(largest, std::abs(x[m_Li[q]]));
            if (!(std::abs(pivot) >= kPivotTolerance * largest) || pivot == 0.0)
            {
                for (int q = m_Lp[j]; q < m_Lp[j + 1]; ++q)
                    x[m_Li[q]] = 0.0;
                return false;
            }
            m_pivot[j] = pivot;
            const double inverse = 1.0 / pivot;
            for (int q = m_Lp[j]; q < m_Lp[j + 1]; ++q)
            {
                m_Lx[q] = x[m_Li[q]] * inverse;
                x[m_Li[q]] = 0.0;
            }
        }
        return true;
    }

    void PrintErrorMessage() override { std::fprintf(stderr, "SparseLU: %s\n", m_engine.lastErrorMessage().c_str()); }

    Eigen::SparseLU<chrono::ChSparseMatrix, Eigen::COLAMDOrdering<int>> m_engine;
    struct RowEntry
    {
        int col, slot, transposed;   // the constraint row's entry in a variable's column, and its transpose
    };
    bool m_checked = false, m_direct = false;             // direct assembly checked / used
    int m_variables = 0, m_constraints = 0;
    std::vector<int> m_massSlots, m_rowSlots, m_complianceSlots, m_blockSlots;
    size_t m_blocks = 0;
    std::vector<RowEntry> m_rowEntries;
    Eigen::VectorXd m_unit, m_product;
    std::vector<int> m_outer, m_inner;                    // the pattern the ordering was computed for
    bool m_symbolic = false;                              // the structure below matches Eigen's pivots
    std::vector<int> m_rowOf, m_colOf;                    // row / column of A -> of B
    std::vector<int> m_Bp, m_Bi, m_Bv;                    // B by columns: rows, index of the value in A
    std::vector<int> m_Lp, m_Li, m_Up, m_Ui;              // L (unit diagonal) and U (without it) by columns
    std::vector<double> m_Lx, m_Ux, m_pivot, m_x, m_z;
};

// Chrono's mate joint, with its Jacobian computed in fewer operations: Chrono builds three frames (three
// quaternion-to-matrix conversions) and seven 3x3 products per joint and step; the frame 2 axes in body 2,
// R2^T A2 = Rf2^T, are constant. Same quantities (C, Cq, P), same formulas.
class MateJoint : public chrono::ChLinkMateGeneric
{
public:
    using ChLinkMateGeneric::ChLinkMateGeneric;

protected:
    void Update(double time, chrono::UpdateFlags flags) override
    {
        using namespace chrono;
        ChLink::Update(time, flags);
        if (!m_body1 || !m_body2)
            return;
        mask.SetTwoBodiesVariables(&m_body1->Variables(), &m_body2->Variables());
        const ChMatrix33<>& A1 = m_body1->GetRotMat();
        const ChMatrix33<>& A2 = m_body2->GetRotMat();
        const ChMatrix33<>& Rf2 = m_frame2.GetRotMat();
        const ChVector3d d = (m_body1->GetPos() + A1 * m_frame1.GetPos()) - (m_body2->GetPos() + A2 * m_frame2.GetPos());
        const ChMatrix33<> R2t = (A2 * Rf2).transpose();   // world -> frame 2
        const ChMatrix33<> M1 = R2t * A1;                  // body 1 -> frame 2
        const ChMatrix33<> Rf2t = Rf2.transpose();         // body 2 -> frame 2
        const ChVector3d position = R2t * d;               // frame 1 in frame 2
        const ChQuaterniond rotation = (m_body2->GetRot() * m_frame2.GetRot()).GetConjugate() * (m_body1->GetRot() * m_frame1.GetRot());
        const ChMatrix33<> Jr1 = -(M1 * ChStarMatrix33<>(m_frame1.GetPos()));
        const ChMatrix33<> Jr2 = Rf2t * ChStarMatrix33<>(m_frame2.GetPos() + A2.transpose() * d);
        P = 0.5 * (ChMatrix33<>(rotation.e0()) + ChStarMatrix33<>(rotation.GetVector()));
        const ChMatrix33<> Pt = P.transpose();
        const ChMatrix33<> Jw1 = Pt * M1, Jw2 = -(Pt * Rf2t);
        const bool locked[6] = {c_x, c_y, c_z, c_rx, c_ry, c_rz};
        const double residual[6] = {position.x(), position.y(), position.z(), rotation.e1(), rotation.e2(), rotation.e3()};
        int nc = 0;
        for (int k = 0; k < 6; ++k)
        {
            if (!locked[k])
                continue;
            C(nc) = residual[k];
            ChConstraintTwoBodies& c = mask.GetConstraint(nc);
            auto a = c.Get_Cq_a();
            auto b = c.Get_Cq_b();
            if (k < 3)
            {
                a.segment(0, 3) = R2t.row(k);
                a.segment(3, 3) = Jr1.row(k);
                b.segment(0, 3) = -R2t.row(k);
                b.segment(3, 3) = Jr2.row(k);
            }
            else
            {
                a.segment(0, 3).setZero();
                b.segment(0, 3).setZero();
                a.segment(3, 3) = Jw1.row(k - 3);
                b.segment(3, 3) = Jw2.row(k - 3);
            }
            ++nc;
        }
    }
};

int Constraints(const Joint& j)
{
    int c = 0;
    switch (j.type)
    {
    case JointType::Revolute:
    case JointType::Prismatic: c = 5; break;
    case JointType::Cylindrical: c = 4; break;
    case JointType::Spherical:
    case JointType::Planar: c = 3; break;
    case JointType::Fixed: c = 6; break;
    }
    return c + (j.motor.mode == Motor::Mode::Speed ? 1 : 0);
}

ChVector3d V(const Vec3& v)
{
    return ChVector3d(v[0], v[1], v[2]);
}

ChVector3d Unit(const Vec3& v)
{
    const ChVector3d a = V(v);
    return a.Length() > 1e-12 ? a.GetNormalized() : ChVector3d(0, 0, 1);
}

bool Rotational(JointType t)
{
    return t == JointType::Revolute || t == JointType::Cylindrical;
}

}  // namespace

int Mobility(const Model& model)
{
    auto between = [&](const Joint& j) {   // a joint between two ground bodies constrains nothing that moves
        const bool fixed0 = j.body0 < 0 || model.bodies[size_t(j.body0)].fixed;
        const bool fixed1 = j.body1 < 0 || model.bodies[size_t(j.body1)].fixed;
        return !(fixed0 && fixed1);
    };
    // Planar mechanism (every revolute axis parallel to one normal, every prismatic axis across it, fixed
    // joints allowed): the planar count, since the spatial one counts its constraints twice over.
    ChVector3d normal(0, 0, 0);
    bool planar = !model.joints.empty();
    for (const Joint& j : model.joints)
        if (j.type == JointType::Revolute)
        {
            const ChVector3d a = Unit(j.axis);
            if (normal.Length() == 0.0)
                normal = a;
            else if (std::abs(std::abs(a.Dot(normal)) - 1.0) > 1e-6)
                planar = false;
        }
        else if (j.type != JointType::Prismatic && j.type != JointType::Fixed)
            planar = false;
    if (planar && normal.Length() > 0.0)
        for (const Joint& j : model.joints)
            if (j.type == JointType::Prismatic && std::abs(Unit(j.axis).Dot(normal)) > 1e-6)
                planar = false;
    planar = planar && normal.Length() > 0.0;
    int dof = 0;
    for (const Body& b : model.bodies)
        dof += b.fixed ? 0 : (planar ? 3 : 6);
    for (const Joint& j : model.joints)
        if (between(j))
            dof -= planar ? (j.type == JointType::Fixed ? 3 : 2) + (j.motor.mode == Motor::Mode::Speed ? 1 : 0) : Constraints(j);
    return dof - int(model.couplings.size());
}

struct Multibody::Impl
{
    chrono::ChSystemNSC system;
    std::shared_ptr<chrono::ChBody> ground;
    std::vector<std::shared_ptr<chrono::ChBody>> bodies;
    std::vector<unsigned int> accumulators;
    Model model;
    // Per joint: attachment offsets in each body (from its centroid, rest axes), axis in body1's rest
    // axes, the unwrapped coordinate, the load for the next step and the damping of its stops.
    struct JointState
    {
        ChVector3d offset0, offset1, axis;
        double coordinate = 0.0, previousAngle = 0.0, load = 0.0, stopStiffness = 0.0, stopDamping = 0.0;
        double torsionStiffness = 0.0, torsionPreload = 0.0, torsionDamping = 0.0;   // its torsion springs, summed
        std::shared_ptr<PairStiffness> stop;                                        // its stops' stiffness (implicit)
    };
    std::vector<JointState> joints;
    std::map<size_t, std::shared_ptr<chrono::ChShaft>> shafts;   // joint -> its coordinate as a 1D shaft (couplings)
    std::vector<double> contactDamping, contactForce;
    std::vector<std::vector<ChVector3d>> contactProfile;          // per contact: outline points (profile body local)
    struct PointGroup
    {
        ChVector3d center;
        double radius;
        size_t begin, end;
    };
    std::vector<std::vector<PointGroup>> contactGroups;           // per contact: runs of its outline points
    double stableStep = 2.0e-4;

    // A touch: each side's outline in its body's rest axes (from the centroid) and on the plane (u, v).
    struct EdgeBox
    {
        double u0, u1, v0, v1;
        size_t begin, end;   // edges i in [begin, end): from point i to the next of its loop
    };
    struct Side
    {
        std::vector<ChVector3d> points;
        std::vector<std::array<double, 2>> flat;
        std::vector<size_t> loops;   // loop k = [loops[k], loops[k + 1])
        std::vector<size_t> next;    // the next point along its loop
        std::vector<PointGroup> groups;
        std::vector<EdgeBox> boxes;
        double cu = 0.0, cv = 0.0, radius = 0.0;   // bounding circle on the plane
    };
    struct TouchState
    {
        Side side[2];
        ChVector3d u, v;   // the plane
        double stiffness = 0.0, damping = 0.0, force = 0.0;
        std::shared_ptr<PairStiffness> implicit;
    };
    // A body's half of a Jacobian: a force along `n` at the world point `at` (or, `turn`, a torque about `n`), in the
    // body's speed and angular speed (its axes); zero for the ground.
    Eigen::Matrix<double, 6, 1> Half(int index, const ChVector3d& n, const ChVector3d& at, bool turn) const
    {
        Eigen::Matrix<double, 6, 1> h = Eigen::Matrix<double, 6, 1>::Zero();
        if (!Moving(index))
            return h;
        const auto b = BodyOf(index);
        const ChVector3d r = turn ? b->GetRot().RotateBack(n) : b->GetRot().RotateBack((at - b->GetPos()).Cross(n));
        if (!turn)
            h.head<3>() << n.x(), n.y(), n.z();
        h.tail<3>() << r.x(), r.y(), r.z();
        return h;
    }
    // The Jacobian of a force along `n` on body `first` (and its reaction on `second`), in the order of a pair item
    // made for (body0, body1).
    PairStiffness::Jacobian PairJacobian(int body0, int first, int second, const ChVector3d& n, const ChVector3d& at, bool turn) const
    {
        PairStiffness::Jacobian j;
        const Eigen::Matrix<double, 6, 1> a = Half(first, n, at, turn), b = -Half(second, n, at, turn);
        if (first == body0)
            j << a, b;
        else
            j << b, a;
        return j;
    }
    std::vector<TouchState> touches;

    // Even-odd: is (pu, pv) in the material of the side?
    static bool Inside(const Side& s, double pu, double pv)
    {
        bool in = false;
        for (const EdgeBox& b : s.boxes)
        {
            if (pv < b.v0 || pv > b.v1 || pu > b.u1)
                continue;
            for (size_t i = b.begin; i < b.end; ++i)
            {
                const auto& a = s.flat[i];
                const auto& c = s.flat[s.next[i]];
                if ((a[1] > pv) != (c[1] > pv) && pu < (c[0] - a[0]) * (pv - a[1]) / (c[1] - a[1]) + a[0])
                    in = !in;
            }
        }
        return in;
    }
    // Distance to the nearest edge of the side and that edge's outward normal (the material is on the left).
    static double Nearest(const Side& s, double pu, double pv, double& nu, double& nv)
    {
        double best = 1e300;
        for (const EdgeBox& b : s.boxes)
        {
            const double du = std::max({b.u0 - pu, 0.0, pu - b.u1}), dv = std::max({b.v0 - pv, 0.0, pv - b.v1});
            if (du * du + dv * dv >= best * best)
                continue;
            for (size_t i = b.begin; i < b.end; ++i)
            {
                const auto& a = s.flat[i];
                const auto& c = s.flat[s.next[i]];
                const double eu = c[0] - a[0], ev = c[1] - a[1], len2 = eu * eu + ev * ev;
                if (len2 <= 0.0)
                    continue;
                const double t = std::clamp(((pu - a[0]) * eu + (pv - a[1]) * ev) / len2, 0.0, 1.0);
                const double d = std::hypot(pu - a[0] - t * eu, pv - a[1] - t * ev);
                if (d < best)
                {
                    best = d;
                    const double len = std::sqrt(len2);
                    nu = ev / len;
                    nv = -eu / len;
                }
            }
        }
        return best;
    }
    // Mass a body opposes to a push along `n` at the world point `at`: its inertia about the joint it turns on over
    // the push's lever arm squared, or its mass.
    double MassAlong(int index, const ChVector3d& at, const ChVector3d& n) const
    {
        if (!Moving(index))
            return 1e30;
        for (size_t j = 0; j < model.joints.size(); ++j)
            if (Rotational(model.joints[j].type) && (model.joints[j].body0 == index || model.joints[j].body1 == index))
            {
                const ChVector3d axis = AxisNow(j), point = Anchor(j, model.joints[j].body0 == index ? 0 : 1);
                const double lever = std::abs((at - point).Cross(n).Dot(axis));
                return lever > 1e-9 ? EffectiveMass(index, true, Unit(model.joints[j].axis), V(model.joints[j].point)) / (lever * lever) : 1e30;
            }
        return EffectiveMass(index, false, {}, {});
    }
    void ApplyTouches(double dt);

    bool Moving(int index) const { return index >= 0 && !model.bodies[size_t(index)].fixed; }
    std::shared_ptr<chrono::ChBody> BodyOf(int index) const { return index < 0 ? ground : bodies[size_t(index)]; }
    ChVector3d Centroid(int index) const { return index < 0 ? ChVector3d(0, 0, 0) : V(model.bodies[size_t(index)].centroid); }
    // A point given at rest (model frame) as it is now.
    ChVector3d World(int index, const ChVector3d& rest) const
    {
        const auto b = BodyOf(index);
        return b->GetPos() + b->GetRot().Rotate(rest - Centroid(index));
    }
    ChVector3d Velocity(int index, const ChVector3d& world) const
    {
        const auto b = BodyOf(index);
        return b->GetPosDt() + b->GetAngVelParent().Cross(world - b->GetPos());
    }
    // Mass felt along a direction (sliding) or about an axis through a point (rotation) by a moving body.
    double EffectiveMass(int index, bool rotation, const ChVector3d& axis, const ChVector3d& point) const
    {
        if (!Moving(index))
            return 1e30;
        const Body& b = model.bodies[size_t(index)];
        if (!rotation)
            return std::max(b.mass, 1e-12);
        const auto& I = b.inertia;
        const double a[3] = {axis.x(), axis.y(), axis.z()};
        const double t[3][3] = {{I[0], I[5], I[4]}, {I[5], I[1], I[3]}, {I[4], I[3], I[2]}};
        double about = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                about += a[r] * t[r][c] * a[c];
        const ChVector3d d = Centroid(index) - point;
        const double arm = (d - axis * d.Dot(axis)).Length();
        return std::max(about + b.mass * arm * arm, 1e-20);   // floors far below a watch's parts
    }

    ChVector3d AxisNow(size_t j) const { return BodyOf(model.joints[j].body1)->GetRot().Rotate(joints[j].axis); }
    double RawAngle(size_t j) const
    {
        // Rotation of body0 relative to body1, about the joint axis (body1 axes).
        const Joint& joint = model.joints[j];
        const ChQuaterniond q0 = BodyOf(joint.body0)->GetRot(), q1 = BodyOf(joint.body1)->GetRot();
        const ChQuaterniond rel = q1.GetConjugate() * q0;
        const double s = rel.e1() * joints[j].axis.x() + rel.e2() * joints[j].axis.y() + rel.e3() * joints[j].axis.z();
        return 2.0 * std::atan2(s, rel.e0());
    }
    ChVector3d Anchor(size_t j, int side) const
    {
        const Joint& joint = model.joints[j];
        const auto b = BodyOf(side == 0 ? joint.body0 : joint.body1);
        return b->GetPos() + b->GetRot().Rotate(side == 0 ? joints[j].offset0 : joints[j].offset1);
    }
    double Displacement(size_t j) const { return (Anchor(j, 0) - Anchor(j, 1)).Dot(AxisNow(j)); }
    double Rate(size_t j) const
    {
        const Joint& joint = model.joints[j];
        const ChVector3d axis = AxisNow(j);
        if (Rotational(joint.type))
            return (BodyOf(joint.body0)->GetAngVelParent() - BodyOf(joint.body1)->GetAngVelParent()).Dot(axis);
        const ChVector3d p0 = Anchor(j, 0), p1 = Anchor(j, 1);
        return (Velocity(joint.body0, p0) - Velocity(joint.body1, p1)).Dot(axis);
    }

    // Torque about / force along the joint axis on body0, the reaction on body1.
    void ApplyAlong(size_t j, double value)
    {
        const Joint& joint = model.joints[j];
        const ChVector3d load = AxisNow(j) * value;
        for (int side = 0; side < 2; ++side)
        {
            const int index = side == 0 ? joint.body0 : joint.body1;
            if (!Moving(index))
                continue;
            const auto& body = bodies[size_t(index)];
            const ChVector3d l = side == 0 ? load : -load;
            if (Rotational(joint.type))
                body->AccumulateTorque(accumulators[size_t(index)], l, false);
            else
                body->AccumulateForce(accumulators[size_t(index)], l, Anchor(j, side), false);
        }
    }
    void ApplyForce(int index, const ChVector3d& force, const ChVector3d& at)
    {
        if (Moving(index))
            bodies[size_t(index)]->AccumulateForce(accumulators[size_t(index)], force, at, false);
    }

    // Starts joint j at `value` instead of the drawn position: its moving body, with the bodies fixed to it, turns about
    // (slides along) the joint. Before any step: the joints and touches are anchored in the bodies, so they follow.
    void Place(size_t j, double value)
    {
        const Joint& joint = model.joints[j];
        const bool first = Moving(joint.body0);
        const int mover = first ? joint.body0 : joint.body1;
        if (!Moving(mover))
            return;
        std::set<int> group{mover};
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const Joint& f : model.joints)
                if (f.type == JointType::Fixed && &f != &joint)
                    for (const auto& [a, b] : {std::pair{f.body0, f.body1}, std::pair{f.body1, f.body0}})
                        if (group.contains(a) && Moving(b) && b != (first ? joint.body1 : joint.body0) && group.insert(b).second)
                            grew = true;
        }
        const double v = first ? value : -value;
        const ChVector3d axis = AxisNow(j), point = Anchor(j, first ? 1 : 0);
        const ChQuaterniond q = QuatFromAngleAxis(v, axis);
        for (const int b : group)
        {
            const auto& body = bodies[size_t(b)];
            if (Rotational(joint.type))
            {
                body->SetPos(point + q.Rotate(body->GetPos() - point));
                body->SetRot(q * body->GetRot());
            }
            else
                body->SetPos(body->GetPos() + axis * v);
        }
    }

    // The joint coordinate as a 1D shaft (rotation: rad, sliding: m), for couplings.
    std::shared_ptr<chrono::ChShaft> JointShaft(size_t j)
    {
        using namespace chrono;
        if (auto it = shafts.find(j); it != shafts.end())
            return it->second;
        const Joint& joint = model.joints[j];
        const bool rotation = Rotational(joint.type);
        // Nearly massless next to the bodies on the joint (1e-7 of them; never more than 1e-9 kg m^2 / 1e-7 kg).
        const double felt = std::min(EffectiveMass(joint.body0, rotation, joints[j].axis, V(joint.point)),
                                     EffectiveMass(joint.body1, rotation, joints[j].axis, V(joint.point)));
        const double inertia = std::min(rotation ? 1e-9 : 1e-7, 1e-7 * felt);
        auto shaftOf = [&](int index, const ChVector3d& offset) -> std::shared_ptr<ChShaft> {
            if (!Moving(index))
                return nullptr;
            auto shaft = chrono_types::make_shared<ChShaft>();
            shaft->SetInertia(inertia);
            system.AddShaft(shaft);
            // Bodies start with the rest (identity) rotation: the axis is the same in their local axes.
            if (rotation)
            {
                auto c = chrono_types::make_shared<ChShaftBodyRotation>();
                c->Initialize(shaft, bodies[size_t(index)], joints[j].axis);
                system.Add(c);
            }
            else
            {
                auto c = chrono_types::make_shared<ChShaftBodyTranslation>();
                c->Initialize(shaft, bodies[size_t(index)], joints[j].axis, offset);
                system.Add(c);
            }
            return shaft;
        };
        auto s0 = shaftOf(joint.body0, joints[j].offset0), s1 = shaftOf(joint.body1, joints[j].offset1);
        std::shared_ptr<ChShaft> relative = s0;
        if (s1)
        {
            relative = chrono_types::make_shared<ChShaft>();
            relative->SetInertia(inertia);
            system.AddShaft(relative);
            if (s0)
            {
                // relative - s0 + s1 = 0
                auto p = chrono_types::make_shared<ChShaftsPlanetary>();
                p->Initialize(relative, s0, s1);
                p->SetTransmissionRatios(1.0, -1.0, 1.0);
                system.Add(p);
            }
            else
            {
                auto g = chrono_types::make_shared<ChShaftsGear>();
                g->Initialize(s1, relative);
                g->SetTransmissionRatio(-1.0);
                system.Add(g);
            }
        }
        shafts[j] = relative;
        return relative;
    }
};

Multibody::Multibody(const Model& model) : m(std::make_unique<Impl>())
{
    using namespace chrono;
    m->model = model;
    ChSystemNSC& sys = m->system;
    sys.SetGravitationalAcceleration(V(model.gravity));
    // Sparse LU (several times faster than QR; the redundant constraints are made regular below). The pattern
    // of the system never changes (no contacts in Chrono, fixed joints and couplings): analysed once.
    auto solver = chrono_types::make_shared<SolverLU>();
    solver->UseSparsityPatternLearner(true);
    solver->LockSparsityPattern(true);
    sys.SetSolver(solver);
    sys.SetSystemDescriptor(chrono_types::make_shared<Descriptor>());

    m->ground = chrono_types::make_shared<ChBody>();
    m->ground->SetFixed(true);
    sys.AddBody(m->ground);

    for (const Body& b : model.bodies)
    {
        auto body = chrono_types::make_shared<ChBody>();
        body->SetPos(V(b.centroid));
        body->SetMass(std::max(b.mass, 1e-9));
        const double floor = std::max(1e-18, b.mass * 1e-8);   // relative: a watch's wheels are ~1e-11 kg m^2
        body->SetInertiaXX(ChVector3d(std::max(b.inertia[0], floor), std::max(b.inertia[1], floor), std::max(b.inertia[2], floor)));
        body->SetInertiaXY(ChVector3d(b.inertia[5], b.inertia[4], b.inertia[3]));   // Ixy, Ixz, Iyz
        body->SetFixed(b.fixed);
        m->accumulators.push_back(body->AddAccumulator());
        sys.AddBody(body);
        m->bodies.push_back(body);
    }

    auto stable = [&](double stiffness, double mass) {
        if (stiffness > 0.0 && mass < 1e29)
            m->stableStep = std::min(m->stableStep, 0.4 / std::sqrt(stiffness / mass));
    };
    for (const Joint& j : model.joints)
    {
        auto b0 = m->BodyOf(j.body0), b1 = m->BodyOf(j.body1);
        const ChVector3d axis = Unit(j.axis);
        // Chrono's joints and motors act along / about the Z axis of the joint frame.
        const ChFrame<> frame(V(j.point), QuatFromVec2Vec(ChVector3d(0, 0, 1), axis));
        std::shared_ptr<ChLinkBase> link;
        const bool rotation = Rotational(j.type);
        if (j.motor.mode == Motor::Mode::Speed && rotation)
        {
            auto speed = chrono_types::make_shared<ChLinkMotorRotationSpeed>();
            speed->SetSpeedFunction(chrono_types::make_shared<ChFunctionConst>(j.motor.value));
            speed->SetSpindleConstraint(j.type == JointType::Cylindrical ? ChLinkMotorRotation::SpindleConstraint::CYLINDRICAL
                                                                          : ChLinkMotorRotation::SpindleConstraint::REVOLUTE);
            speed->Initialize(b0, b1, frame);
            link = speed;
        }
        else if (j.motor.mode == Motor::Mode::Speed && j.type == JointType::Prismatic)
        {
            auto speed = chrono_types::make_shared<ChLinkMotorLinearSpeed>();
            speed->SetSpeedFunction(chrono_types::make_shared<ChFunctionConst>(j.motor.value));
            speed->SetGuideConstraint(ChLinkMotorLinear::GuideConstraint::PRISMATIC);
            speed->Initialize(b0, b1, frame);
            link = speed;
        }
        else
        {
            // Torque motors are loads applied every step (their characteristic depends on the speed). Mate joints
            // (the locked directions): the constraints of Chrono's lock joints at a fraction of their update cost.
            auto mate = [&](bool x, bool y, bool z, bool rx, bool ry, bool rz) { return chrono_types::make_shared<MateJoint>(x, y, z, rx, ry, rz); };
            std::shared_ptr<ChLinkMateGeneric> joint;
            switch (j.type)
            {
            case JointType::Revolute: joint = mate(true, true, true, true, true, false); break;
            case JointType::Prismatic: joint = mate(true, true, false, true, true, true); break;
            case JointType::Cylindrical: joint = mate(true, true, false, true, true, false); break;
            case JointType::Spherical: joint = mate(true, true, true, false, false, false); break;
            case JointType::Planar: joint = mate(false, false, true, true, true, false); break;
            case JointType::Fixed: joint = mate(true, true, true, true, true, true); break;
            }
            joint->Initialize(b0, b1, frame);
            link = joint;
        }
        sys.AddLink(link);
        Impl::JointState state;
        state.offset0 = V(j.point) - m->Centroid(j.body0);
        state.offset1 = V(j.point) - m->Centroid(j.body1);
        state.axis = axis;
        if (j.lower || j.upper)
        {
            const double mass = std::min(m->EffectiveMass(j.body0, rotation, axis, V(j.point)), m->EffectiveMass(j.body1, rotation, axis, V(j.point)));
            state.stopStiffness = std::min(rotation ? model.stopRotationalStiffness : model.stopStiffness, AutoStiffness(mass));
            state.stopDamping = mass < 1e29 ? 2.0 * kDampingRatio * std::sqrt(state.stopStiffness * mass) : 0.0;
            state.stop = chrono_types::make_shared<PairStiffness>(&b0->Variables(), &b1->Variables());
            sys.Add(state.stop);
            stable(state.stopStiffness, mass);
        }
        m->joints.push_back(state);
    }

    for (const Coupling& c : model.couplings)
    {
        auto g = chrono_types::make_shared<ChShaftsGear>();
        g->Initialize(m->JointShaft(size_t(c.joint0)), m->JointShaft(size_t(c.joint1)));
        g->SetTransmissionRatio(c.ratio);
        sys.Add(g);
    }

    for (const Spring& s : model.springs)
        stable(s.stiffness, std::min(m->EffectiveMass(s.body0, false, {}, {}), m->EffectiveMass(s.body1, false, {}, {})));
    for (const Contact& c : model.contacts)
    {
        const double mass = std::min(m->EffectiveMass(c.follower, false, {}, {}), m->EffectiveMass(c.body, false, {}, {}));
        m->contactDamping.push_back(c.damping >= 0.0 ? c.damping : mass < 1e29 ? 2.0 * kDampingRatio * std::sqrt(c.stiffness * mass) : 0.0);
        m->contactForce.push_back(0.0);
        std::vector<ChVector3d> local;   // bodies start unrotated, at their centroid
        for (const Vec3& p : c.profile)
            local.push_back(V(p) - m->Centroid(c.body));
        // Runs of neighbouring outline points in a bounding sphere: the contact search skips whole runs.
        std::vector<Impl::PointGroup> groups;
        for (size_t begin = 0; begin < local.size(); begin += 32)
        {
            Impl::PointGroup g{{}, 0.0, begin, std::min(local.size(), begin + 32)};
            for (size_t i = g.begin; i < g.end; ++i)
                g.center += local[i] / double(g.end - g.begin);
            for (size_t i = g.begin; i < g.end; ++i)
                g.radius = std::max(g.radius, (local[i] - g.center).Length() * (1.0 + 1e-12));
            groups.push_back(g);
        }
        m->contactGroups.push_back(std::move(groups));
        m->contactProfile.push_back(std::move(local));
        stable(c.stiffness, mass);
    }
    for (const TorsionSpring& t : model.torsions)
    {
        Impl::JointState& js = m->joints[size_t(t.joint)];
        const Joint& j = model.joints[size_t(t.joint)];
        js.torsionStiffness += t.stiffness;
        js.torsionPreload += t.stiffness * t.free;
        js.torsionDamping += t.damping;
        const bool rotation = Rotational(j.type);
        stable(t.stiffness, std::min(m->EffectiveMass(j.body0, rotation, js.axis, V(j.point)), m->EffectiveMass(j.body1, rotation, js.axis, V(j.point))));
    }
    for (const Touch& t : model.touches)
    {
        Impl::TouchState ts;
        const ChVector3d n = Unit(t.normal);
        const ChVector3d helper = std::abs(n.x()) < 0.9 ? ChVector3d(1, 0, 0) : ChVector3d(0, 1, 0);
        ts.u = n.Cross(helper).GetNormalized();
        ts.v = n.Cross(ts.u);

        for (int k = 0; k < 2; ++k)
        {
            Impl::Side& s = ts.side[k];
            const int body = k == 0 ? t.body0 : t.body1;

            for (const std::vector<Vec3>& loop : k == 0 ? t.outline0 : t.outline1)
            {
                s.loops.push_back(s.points.size());
                for (size_t i = 0; i < loop.size(); ++i)
                {

                    s.points.push_back(V(loop[i]) - m->Centroid(body));   // bodies start unrotated, at their centroid
                    s.flat.push_back({s.points.back().Dot(ts.u), s.points.back().Dot(ts.v)});
                    s.next.push_back(i + 1 < loop.size() ? s.points.size() : s.loops.back());
                }
            }
            s.loops.push_back(s.points.size());
            for (size_t begin = 0; begin < s.points.size(); begin += 16)
            {
                const size_t end = std::min(s.points.size(), begin + 16);
                Impl::PointGroup g{{}, 0.0, begin, end};
                Impl::EdgeBox b{1e300, -1e300, 1e300, -1e300, begin, end};
                for (size_t i = begin; i < end; ++i)
                {
                    g.center += s.points[i] / double(end - begin);
                    for (const auto& p : {s.flat[i], s.flat[s.next[i]]})
                        b.u0 = std::min(b.u0, p[0]), b.u1 = std::max(b.u1, p[0]), b.v0 = std::min(b.v0, p[1]), b.v1 = std::max(b.v1, p[1]);
                }
                for (size_t i = begin; i < end; ++i)
                    g.radius = std::max(g.radius, (s.points[i] - g.center).Length() * (1.0 + 1e-12));
                s.groups.push_back(g);
                s.boxes.push_back(b);
            }
            for (const auto& p : s.flat)
                s.cu += p[0] / double(s.flat.size()), s.cv += p[1] / double(s.flat.size());
            for (const auto& p : s.flat)
                s.radius = std::max(s.radius, std::hypot(p[0] - s.cu, p[1] - s.cv) * (1.0 + 1e-12));
        }
        // Soft contact (as Box2D's soft step): per contact and step, from the pair's reduced mass at the contact point
        // along its normal, a spring at a quarter of the step rate, heavily damped; implicit. Stable for any masses
        // (a heavy balance's pin in a light fork), it adds no energy (impacts are inelastic), and the penetration
        // under steady loads stays a fraction of a micrometre in a watch. A given stiffness replaces the spring.
        ts.stiffness = t.stiffness;
        ts.damping = t.damping;
        ts.implicit = chrono_types::make_shared<PairStiffness>(&m->BodyOf(t.body0)->Variables(), &m->BodyOf(t.body1)->Variables());
        sys.Add(ts.implicit);
        m->touches.push_back(std::move(ts));
        m->stableStep = std::min(m->stableStep, kTouchStep);
    }
    m->stableStep = std::max(m->stableStep, 1e-7);

    // Joints that start away from the drawn position (a value, or the local time); then every joint's coordinate from
    // where the bodies are (a joint carried along reads its new angle).
    const double clock = LocalClockSeconds();
    std::map<size_t, double> placed;
    for (size_t j = 0; j < model.joints.size(); ++j)
    {
        const Joint& joint = model.joints[j];
        double value = joint.start;
        if (joint.clockPeriod != 0.0 && Rotational(joint.type))
            value += 2.0 * std::numbers::pi * std::fmod(clock, std::abs(joint.clockPeriod)) / joint.clockPeriod;
        if (value != 0.0 && (Rotational(joint.type) || joint.type == JointType::Prismatic))
        {
            m->Place(j, value);
            placed[j] = value;
        }
    }
    for (size_t j = 0; j < model.joints.size(); ++j)
        if (Rotational(model.joints[j].type))
        {
            Impl::JointState& js = m->joints[j];
            js.previousAngle = m->RawAngle(j);
            js.coordinate = placed.contains(j) ? placed[j] : js.previousAngle;
        }
        else if (model.joints[j].type == JointType::Prismatic)
            m->joints[j].coordinate = m->Displacement(j);

    // Redundant constraints (a planar linkage made of spatial joints, a gear train closing a loop) make the
    // system singular, and which ones are redundant changes as the mechanism moves (dead centres). A tiny
    // compliance on every constraint keeps it regular everywhere: the error it allows is ~1e-10 m/s per N.
    sys.Setup();
    sys.Update();
    sys.DescriptorPrepareInject();
    for (ChConstraint* c : sys.GetSystemDescriptor()->GetConstraints())
        c->SetComplianceTerm(1e-10);
}

Multibody::~Multibody() = default;

double LocalClockSeconds()
{
    using namespace std::chrono;
    const auto local = zoned_time(current_zone(), system_clock::now()).get_local_time();
    return duration<double>(local.time_since_epoch()).count();
}

// Each side's outline points against the other side's material: every run of consecutive points inside it is
// one touching patch, pushed out at its deepest point along the normal of the edge it is nearest to.
void Multibody::Impl::ApplyTouches(double dt)
{
    for (size_t t = 0; t < touches.size(); ++t)
    {
        TouchState& ts = touches[t];
        ts.force = 0.0;
        ts.implicit->Clear();
        struct Found
        {
            int first, second;   // pushed along +n / its reaction
            ChVector3d at, n;
            double depth;
        };
        std::vector<Found> found;
        for (int a = 0; a < 2; ++a)
        {
            const int ia = a == 0 ? model.touches[t].body0 : model.touches[t].body1;
            const int ib = a == 0 ? model.touches[t].body1 : model.touches[t].body0;
            const Side& A = ts.side[a];
            const Side& B = ts.side[1 - a];
            const auto bodyA = BodyOf(ia), bodyB = BodyOf(ib);
            const ChQuaterniond qa = bodyA->GetRot(), qb = bodyB->GetRot();
            const ChVector3d pa = bodyA->GetPos(), pb = bodyB->GetPos();
            // A point of A in B's rest axes, on the plane.
            auto inB = [&](const ChVector3d& local, double& u, double& v) {
                const ChVector3d w = qb.RotateBack(pa + qa.Rotate(local) - pb);
                u = w.Dot(ts.u);
                v = w.Dot(ts.v);
            };
            struct Patch
            {
                size_t first, last, deepest, loop;
                double depth, nu, nv;
            };
            std::vector<Patch> patches;
            Patch open{0, 0, 0, 0, 0.0, 0.0, 0.0};
            bool inside = false;
            auto close = [&] {
                if (inside)
                    patches.push_back(open);
                inside = false;
            };
            size_t loop = 1;
            for (const PointGroup& g : A.groups)
            {
                double gu, gv;
                inB(g.center, gu, gv);
                if (std::hypot(gu - B.cu, gv - B.cv) > B.radius + g.radius)
                {
                    close();
                    continue;
                }
                for (size_t i = g.begin; i < g.end; ++i)
                {
                    for (; loop + 1 < A.loops.size() && i >= A.loops[loop]; ++loop)   // a new loop: a new patch
                        close();
                    double u, v, eu = 0.0, ev = 0.0;
                    inB(A.points[i], u, v);
                    if (!Inside(B, u, v))
                    {
                        close();
                        continue;
                    }
                    const double d = Nearest(B, u, v, eu, ev);
                    if (!inside)
                        open = {i, i, i, loop - 1, d, eu, ev}, inside = true;
                    open.last = i;
                    if (d > open.depth)
                        open.deepest = i, open.depth = d, open.nu = eu, open.nv = ev;
                }
            }
            close();
            // A patch across the start of its loop is one patch, not two.
            for (size_t p = 0; p < patches.size(); ++p)
                for (size_t q = 0; q < patches.size(); ++q)
                    if (p != q && patches[p].loop == patches[q].loop && patches[p].first == A.loops[patches[p].loop] &&
                        patches[q].last + 1 == A.loops[patches[q].loop + 1] && patches[p].depth > 0.0 && patches[q].depth > 0.0)
                    {
                        if (patches[q].depth > patches[p].depth)
                            patches[p] = patches[q];
                        patches[q].depth = 0.0;
                    }
            for (const Patch& patch : patches)
                if (patch.depth > 0.0)
                    found.push_back({ia, ib, pa + qa.Rotate(A.points[patch.deepest]), qb.Rotate(ts.u * patch.nu + ts.v * patch.nv), patch.depth});
        }
        // The same contact seen from both sides (a corner in a face, the face's points in the corner) is one
        // contact: the deeper view is kept.
        for (size_t p = 0; p < found.size(); ++p)
            for (size_t q = p + 1; q < found.size(); ++q)
                if (found[p].first != found[q].first && found[p].depth > 0.0 && found[q].depth > 0.0 && found[p].n.Dot(found[q].n) < -0.5 &&
                    (found[p].at - found[q].at).Length() < 2.0 * std::max(found[p].depth, found[q].depth) + 2e-5)
                    (found[p].depth < found[q].depth ? found[p] : found[q]).depth = 0.0;
        // Each contact pushed out at its deepest point: the elastic force here, the spring and its damping in the
        // step's matrix, from the reduced mass the contact moves.
        const double w = 2.0 * std::numbers::pi * kTouchRate / dt;
        for (const Found& c : found)
        {
            if (c.depth <= 0.0)
                continue;
            const double ma = MassAlong(c.first, c.at, c.n), mb = MassAlong(c.second, c.at, c.n);
            const double reduced = ma < 1e29 && mb < 1e29 ? ma * mb / (ma + mb) : std::min(ma, mb);
            if (reduced >= 1e29)
                continue;
            const double k = ts.stiffness > 0.0 ? ts.stiffness : reduced * w * w;
            const double damping = ts.damping >= 0.0 ? ts.damping : 2.0 * kTouchDamping * std::sqrt(k * reduced);
            const double f = k * c.depth;
            ts.implicit->Add(PairJacobian(model.touches[t].body0, c.first, c.second, c.n, c.at, false), k, damping);
            ApplyForce(c.first, c.n * f, c.at);
            ApplyForce(c.second, -c.n * f, c.at);
            ts.force += f;
        }
    }
}

void Multibody::Step(double dt)
{
    Impl& s = *m;
    for (size_t i = 0; i < s.bodies.size(); ++i)
        s.bodies[i]->EmptyAccumulator(s.accumulators[i]);
    const double t = s.system.GetChTime();
    for (size_t j = 0; j < s.joints.size(); ++j)
    {
        Impl::JointState& js = s.joints[j];
        const Joint& joint = s.model.joints[j];
        double value = js.load;
        js.load = 0.0;
        const bool timed = joint.motor.duration <= 0.0 || t < joint.motor.duration;
        const double rate = (joint.motor.mode == Motor::Mode::Torque || joint.lower || joint.upper || joint.load.constant != 0.0 ||
                             joint.load.linear != 0.0 || joint.load.quadratic != 0.0 || js.torsionDamping != 0.0)
                                ? s.Rate(j)
                                : 0.0;
        value += js.torsionPreload - js.torsionStiffness * js.coordinate - js.torsionDamping * rate;
        if (joint.motor.mode == Motor::Mode::Torque && timed)
            value += joint.motor.freeSpeed > 0.0 ? joint.motor.value * std::max(0.0, 1.0 - rate / joint.motor.freeSpeed) : joint.motor.value;
        value -= joint.load.constant * std::tanh(rate / 0.5) + joint.load.linear * rate + joint.load.quadratic * rate * std::abs(rate);
        if (joint.lower || joint.upper)
        {
            // On a stop: its elastic force here, its stiffness and damping in the step's matrix.
            const double k = js.stopStiffness;
            js.stop->Clear();
            const bool below = joint.lower && js.coordinate < *joint.lower, above = joint.upper && js.coordinate > *joint.upper;
            if (below)
                value += k * (*joint.lower - js.coordinate);
            if (above)
                value -= k * (js.coordinate - *joint.upper);
            if (below || above)
                js.stop->Add(s.PairJacobian(joint.body0, joint.body0, joint.body1, s.AxisNow(j), s.Anchor(j, 0), Rotational(joint.type)), k, js.stopDamping);
        }
        if (value != 0.0)
            s.ApplyAlong(j, value);
    }
    for (const Spring& sp : s.model.springs)
    {
        const ChVector3d p0 = s.World(sp.body0, V(sp.point0)), p1 = s.World(sp.body1, V(sp.point1));
        const ChVector3d d = p1 - p0;
        const double length = d.Length();
        if (length < 1e-12)
            continue;
        const ChVector3d u = d / length;
        const double stretchRate = (s.Velocity(sp.body1, p1) - s.Velocity(sp.body0, p0)).Dot(u);
        const double force = sp.stiffness * (sp.freeLength - length) - sp.damping * stretchRate;   // > 0 pushes apart
        s.ApplyForce(sp.body1, u * force, p1);
        s.ApplyForce(sp.body0, -u * force, p0);
    }
    for (size_t c = 0; c < s.model.contacts.size(); ++c)
    {
        const Contact& ct = s.model.contacts[c];
        const auto follower = s.BodyOf(ct.follower), profile = s.BodyOf(ct.body);
        const ChVector3d face = s.World(ct.follower, V(ct.point)), n = follower->GetRot().Rotate(Unit(ct.normal));
        // The face in the profile body's axes: the outline points are used as stored.
        const ChQuaterniond q = profile->GetRot();
        const ChVector3d faceLocal = q.RotateBack(face - profile->GetPos()), nLocal = q.RotateBack(n);
        const double radius2 = ct.radius * ct.radius;
        double depth = 0.0;
        const ChVector3d* deepest = nullptr;
        const std::vector<ChVector3d>& points = s.contactProfile[c];
        for (const Impl::PointGroup& g : s.contactGroups[c])
        {
            // No point of the group can be deeper, or within the face.
            const ChVector3d d = g.center - faceLocal;
            const double along = d.Dot(nLocal);
            if (g.radius - along <= depth || (d - nLocal * along).Length() - g.radius > ct.radius)
                continue;
            for (size_t i = g.begin; i < g.end; ++i)
            {
                const ChVector3d dp = points[i] - faceLocal;
                const double alongP = dp.Dot(nLocal);
                if (-alongP <= depth || (dp - nLocal * alongP).Length2() > radius2)
                    continue;
                depth = -alongP;
                deepest = &points[i];
            }
        }
        s.contactForce[c] = 0.0;
        if (!deepest)
            continue;
        const ChVector3d at = profile->GetPos() + q.Rotate(*deepest);
        const double closing = -(s.Velocity(ct.body, at) - s.Velocity(ct.follower, at)).Dot(n);
        const double force = std::max(0.0, ct.stiffness * depth + s.contactDamping[c] * closing);
        s.contactForce[c] = force;
        s.ApplyForce(ct.follower, -n * force, at);
        s.ApplyForce(ct.body, n * force, at);
    }
    s.ApplyTouches(dt);
    s.system.DoStepDynamics(dt);
    // Unwrapped joint coordinates.
    for (size_t j = 0; j < s.joints.size(); ++j)
    {
        Impl::JointState& js = s.joints[j];
        const JointType type = s.model.joints[j].type;
        if (type == JointType::Prismatic)
            js.coordinate = s.Displacement(j);
        else if (Rotational(type))
        {
            const double raw = s.RawAngle(j);
            double delta = raw - js.previousAngle;
            delta -= 2.0 * std::numbers::pi * std::round(delta / (2.0 * std::numbers::pi));
            js.coordinate += delta;
            js.previousAngle = raw;
        }
    }
}

double Multibody::Time() const
{
    return m->system.GetChTime();
}

double Multibody::StableStep() const
{
    return m->stableStep;
}

BodyState Multibody::State(int body) const
{
    const auto& b = m->bodies[size_t(body)];
    const ChVector3d p = b->GetPos(), v = b->GetPosDt(), w = b->GetAngVelParent();
    const ChQuaterniond q = b->GetRot();
    return {{p.x(), p.y(), p.z()}, {q.e0(), q.e1(), q.e2(), q.e3()}, {v.x(), v.y(), v.z()}, {w.x(), w.y(), w.z()}};
}

double Multibody::Coordinate(int joint) const
{
    return m->joints[size_t(joint)].coordinate;
}

double Multibody::Rate(int joint) const
{
    return m->Rate(size_t(joint));
}

void Multibody::ApplyJointLoad(int joint, double value)
{
    m->joints[size_t(joint)].load += value;
}

double Multibody::ContactForce(int contact) const
{
    return m->contactForce[size_t(contact)];
}

double Multibody::TouchForce(int touch) const
{
    return m->touches[size_t(touch)].force;
}

double Multibody::SpringLength(int spring) const
{
    const Spring& s = m->model.springs[size_t(spring)];
    return (m->World(s.body1, V(s.point1)) - m->World(s.body0, V(s.point0))).Length();
}

double Multibody::SpringForce(int spring) const
{
    const Spring& s = m->model.springs[size_t(spring)];
    return s.stiffness * (s.freeLength - SpringLength(spring));
}

Vec3 Multibody::PointOf(int body, const Vec3& restPoint) const
{
    const ChVector3d w = m->World(body, V(restPoint));
    return {w.x(), w.y(), w.z()};
}

}  // namespace aiad::sim::mb
