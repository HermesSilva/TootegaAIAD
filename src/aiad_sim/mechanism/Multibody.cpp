#include "Multibody.h"

#include <chrono/functions/ChFunctionConst.h>
#include <chrono/physics/ChBody.h>
#include <chrono/physics/ChLinkLock.h>
#include <chrono/physics/ChLinkMotorLinearSpeed.h>
#include <chrono/physics/ChLinkMotorRotationSpeed.h>
#include <chrono/physics/ChShaft.h>
#include <chrono/physics/ChShaftBodyConstraint.h>
#include <chrono/physics/ChShaftsGear.h>
#include <chrono/physics/ChShaftsPlanetary.h>
#include <chrono/physics/ChSystemNSC.h>
#include <chrono/solver/ChDirectSolverLS.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>

namespace aiad::sim::mb
{

using chrono::ChVector3d;
using chrono::ChQuaterniond;

namespace
{

constexpr double kDampingRatio = 0.3;   // contacts and stops

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
        double coordinate = 0.0, previousAngle = 0.0, load = 0.0, stopDamping = 0.0;
    };
    std::vector<JointState> joints;
    std::map<size_t, std::shared_ptr<chrono::ChShaft>> shafts;   // joint -> its coordinate as a 1D shaft (couplings)
    std::vector<double> contactDamping, contactForce;
    std::vector<std::vector<ChVector3d>> contactProfile;          // per contact: outline points (profile body local)
    double stableStep = 2.0e-4;

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
            return std::max(b.mass, 1e-6);
        const auto& I = b.inertia;
        const double a[3] = {axis.x(), axis.y(), axis.z()};
        const double t[3][3] = {{I[0], I[5], I[4]}, {I[5], I[1], I[3]}, {I[4], I[3], I[2]}};
        double about = 0.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                about += a[r] * t[r][c] * a[c];
        const ChVector3d d = Centroid(index) - point;
        const double arm = (d - axis * d.Dot(axis)).Length();
        return std::max(about + b.mass * arm * arm, 1e-9);
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

    // The joint coordinate as a 1D shaft (rotation: rad, sliding: m), for couplings.
    std::shared_ptr<chrono::ChShaft> JointShaft(size_t j)
    {
        using namespace chrono;
        if (auto it = shafts.find(j); it != shafts.end())
            return it->second;
        const Joint& joint = model.joints[j];
        const bool rotation = Rotational(joint.type);
        auto shaftOf = [&](int index, const ChVector3d& offset) -> std::shared_ptr<ChShaft> {
            if (!Moving(index))
                return nullptr;
            auto shaft = chrono_types::make_shared<ChShaft>();
            shaft->SetInertia(rotation ? 1e-9 : 1e-7);
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
            relative->SetInertia(rotation ? 1e-9 : 1e-7);
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
    sys.SetSolver(chrono_types::make_shared<ChSolverSparseQR>());   // tolerates redundant constraints

    m->ground = chrono_types::make_shared<ChBody>();
    m->ground->SetFixed(true);
    sys.AddBody(m->ground);

    for (const Body& b : model.bodies)
    {
        auto body = chrono_types::make_shared<ChBody>();
        body->SetPos(V(b.centroid));
        body->SetMass(std::max(b.mass, 1e-9));
        const double floor = std::max(1e-12, b.mass * 1e-8);
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
        // Chrono's lock joints and motors act along / about the Z axis of the joint frame.
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
            // Torque motors are loads applied every step (their characteristic depends on the speed).
            std::shared_ptr<ChLinkLock> lock;
            switch (j.type)
            {
            case JointType::Revolute: lock = chrono_types::make_shared<ChLinkLockRevolute>(); break;
            case JointType::Prismatic: lock = chrono_types::make_shared<ChLinkLockPrismatic>(); break;
            case JointType::Cylindrical: lock = chrono_types::make_shared<ChLinkLockCylindrical>(); break;
            case JointType::Spherical: lock = chrono_types::make_shared<ChLinkLockSpherical>(); break;
            case JointType::Planar: lock = chrono_types::make_shared<ChLinkLockPlanar>(); break;
            case JointType::Fixed: lock = chrono_types::make_shared<ChLinkLockLock>(); break;
            }
            lock->Initialize(b0, b1, frame);
            link = lock;
        }
        sys.AddLink(link);
        Impl::JointState state;
        state.offset0 = V(j.point) - m->Centroid(j.body0);
        state.offset1 = V(j.point) - m->Centroid(j.body1);
        state.axis = axis;
        if (j.lower || j.upper)
        {
            const double mass = std::min(m->EffectiveMass(j.body0, rotation, axis, V(j.point)), m->EffectiveMass(j.body1, rotation, axis, V(j.point)));
            const double k = rotation ? model.stopRotationalStiffness : model.stopStiffness;
            state.stopDamping = mass < 1e29 ? 2.0 * kDampingRatio * std::sqrt(k * mass) : 0.0;
            stable(k, mass);
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
        std::vector<ChVector3d> local;
        for (const Vec3& p : c.profile)
            local.push_back(V(p));
        m->contactProfile.push_back(std::move(local));
        stable(c.stiffness, mass);
    }
    m->stableStep = std::max(m->stableStep, 1e-6);
}

Multibody::~Multibody() = default;

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
        const bool rotation = Rotational(joint.type);
        double value = js.load;
        js.load = 0.0;
        const bool timed = joint.motor.duration <= 0.0 || t < joint.motor.duration;
        const double rate = (joint.motor.mode == Motor::Mode::Torque || joint.lower || joint.upper || joint.load.constant != 0.0 ||
                             joint.load.linear != 0.0 || joint.load.quadratic != 0.0)
                                ? s.Rate(j)
                                : 0.0;
        if (joint.motor.mode == Motor::Mode::Torque && timed)
            value += joint.motor.freeSpeed > 0.0 ? joint.motor.value * std::max(0.0, 1.0 - rate / joint.motor.freeSpeed) : joint.motor.value;
        value -= joint.load.constant * std::tanh(rate / 0.5) + joint.load.linear * rate + joint.load.quadratic * rate * std::abs(rate);
        if (joint.lower || joint.upper)
        {
            const double k = rotation ? s.model.stopRotationalStiffness : s.model.stopStiffness;
            if (joint.lower && js.coordinate < *joint.lower)
                value += std::max(0.0, k * (*joint.lower - js.coordinate) - js.stopDamping * rate);
            if (joint.upper && js.coordinate > *joint.upper)
                value -= std::max(0.0, k * (js.coordinate - *joint.upper) + js.stopDamping * rate);
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
        const ChVector3d origin = profile->GetPos(), centroid = s.Centroid(ct.body);
        const ChQuaterniond q = profile->GetRot();
        double depth = 0.0;
        ChVector3d at;
        for (const ChVector3d& p : s.contactProfile[c])
        {
            const ChVector3d w = origin + q.Rotate(p - centroid);
            const ChVector3d d = w - face;
            const double along = d.Dot(n);
            if (-along <= depth || (d - n * along).Length() > ct.radius)
                continue;
            depth = -along;
            at = w;
        }
        s.contactForce[c] = 0.0;
        if (depth <= 0.0)
            continue;
        const double closing = -(s.Velocity(ct.body, at) - s.Velocity(ct.follower, at)).Dot(n);
        const double force = std::max(0.0, ct.stiffness * depth + s.contactDamping[c] * closing);
        s.contactForce[c] = force;
        s.ApplyForce(ct.follower, -n * force, at);
        s.ApplyForce(ct.body, n * force, at);
    }
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
