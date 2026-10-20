#include "MechanismStudy.h"

#include "Features.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <set>

namespace aiad::app
{

namespace
{

using kernel::Vec3;
using sim::mb::JointType;
constexpr double kPi = std::numbers::pi;
constexpr double kDeg = 180.0 / kPi;

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Norm(const Vec3& a) { return std::sqrt(Dot(a, a)); }
Vec3 Unit(const Vec3& a)
{
    const double n = Norm(a);
    return n > 1e-15 ? Mul(a, 1.0 / n) : Vec3{0, 0, 1};
}
sim::mb::Vec3 Arr(const Vec3& v) { return {v.x, v.y, v.z}; }
Vec3 FromArr(const sim::mb::Vec3& v) { return {v[0], v[1], v[2]}; }
// Distance from p to the line through c along the unit axis a, and the radial vector.
Vec3 Radial(const Vec3& p, const Vec3& c, const Vec3& a)
{
    const Vec3 d = Sub(p, c);
    return Sub(d, Mul(a, Dot(d, a)));
}
double Wrap180(double a)
{
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0)
        a += 360.0;
    return a - 180.0;
}

std::string Mm(const Vec3& p)
{
    return std::format("({:.1f}, {:.1f}, {:.1f}) mm", p.x * 1e3, p.y * 1e3, p.z * 1e3);
}

JointType TypeOf(const std::string& type)
{
    if (type == "prismatic")
        return JointType::Prismatic;
    if (type == "cylindrical")
        return JointType::Cylindrical;
    if (type == "spherical")
        return JointType::Spherical;
    if (type == "planar")
        return JointType::Planar;
    if (type == "fixed")
        return JointType::Fixed;
    return JointType::Revolute;
}

bool Rotational(JointType t)
{
    return t == JointType::Revolute || t == JointType::Cylindrical;
}

struct Box
{
    Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    bool Empty() const { return lo.x > hi.x; }
    void Add(const Vec3& a, const Vec3& b)
    {
        lo = {std::min(lo.x, a.x), std::min(lo.y, a.y), std::min(lo.z, a.z)};
        hi = {std::max(hi.x, b.x), std::max(hi.y, b.y), std::max(hi.z, b.z)};
    }
    bool Contains(const Vec3& p, double margin) const
    {
        return p.x >= lo.x - margin && p.x <= hi.x + margin && p.y >= lo.y - margin && p.y <= hi.y + margin && p.z >= lo.z - margin &&
               p.z <= hi.z + margin;
    }
};

// Polar frame about an axis (rotors): radius, angle about the axis [deg] and position along it.
struct Polar
{
    Vec3 c, a, e1, e2;
    Polar(const Vec3& centre, const Vec3& axis) : c(centre), a(Unit(axis))
    {
        e1 = Unit(Cross(a, std::abs(a.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
        e2 = Cross(a, e1);
    }
    double Angle(const Vec3& p) const
    {
        const Vec3 r = Radial(p, c, a);
        return std::atan2(Dot(r, e2), Dot(r, e1)) * kDeg;
    }
    double Along(const Vec3& p) const { return Dot(Sub(p, c), a); }
    Vec3 Direction(double deg) const { return Add(Mul(e1, std::cos(deg / kDeg)), Mul(e2, std::sin(deg / kDeg))); }
};

// Angular extent of a set of angles [deg]: its middle (exact for a symmetric part, whatever the
// tessellation) and the extremes relative to it.
void AngularRange(const std::vector<double>& angles, double& middle, double& lo, double& hi)
{
    double sx = 0.0, sy = 0.0;
    for (const double t : angles)
        sx += std::cos(t / kDeg), sy += std::sin(t / kDeg);
    const double mean = std::atan2(sy, sx) * kDeg;
    lo = 1e9, hi = -1e9;
    for (const double t : angles)
    {
        const double d = Wrap180(t - mean);
        lo = std::min(lo, d), hi = std::max(hi, d);
    }
    middle = Wrap180(mean + (lo + hi) / 2.0);
    const double half = (hi - lo) / 2.0;
    lo = -half, hi = half;
}

// Everything read from the document while deriving the study.
struct Reader
{
    const doc::Document& d;
    MechanismStudy& s;
    std::vector<doc::PartInfo> parts;
    std::map<std::string, std::vector<Vec3>> points;   // model-frame vertices of a part's tessellation

    void Error(std::string problem, std::string fix) { s.findings.push_back({true, std::move(problem), std::move(fix)}); }
    void Note(std::string problem, std::string fix) { s.findings.push_back({false, std::move(problem), std::move(fix)}); }

    const doc::PartInfo* Part(const Json& name)
    {
        const std::string n = name.is_string() ? name.get<std::string>() : std::string();
        for (const doc::PartInfo& p : parts)
            if (p.name == n)
                return &p;
        return nullptr;
    }
    // A part named by an element, or an error naming the element.
    const doc::PartInfo* NeedPart(const doc::ElementInfo& e, const char* field)
    {
        const Json& name = e.definition.contains(field) ? e.definition[field] : Json();
        const doc::PartInfo* p = Part(name);
        if (!p)
            Error(std::format("The {} '{}' names the part '{}' in '{}', and there is no such part.", e.type, e.name, name.is_string() ? name.get<std::string>() : name.dump(),
                              field),
                  std::format("Name an existing part (aiad_parts_list) in '{}' (aiad_element_define).", field));
        return p;
    }
    const std::vector<Vec3>& Points(const doc::PartInfo& p)
    {
        auto it = points.find(p.path.GetString());
        if (it != points.end())
            return it->second;
        std::vector<Vec3>& list = points[p.path.GetString()];
        if (const kernel::Mesh* mesh = d.DisplayMesh(p.path))   // cached by the document (shared with the view)
        {
            const Vec3 o = d.BodyOrigin(p.body);
            for (const auto& q : mesh->points)
                list.push_back({q[0] + o.x, q[1] + o.y, q[2] + o.z});
        }
        return list;
    }
    int Body(const std::string& name) const
    {
        const auto it = std::find(s.bodies.begin(), s.bodies.end(), name);
        return it == s.bodies.end() ? -1 : int(it - s.bodies.begin());
    }
    bool Moving(int body) const { return body >= 0 && !s.machine.mechanism.bodies[size_t(body)].fixed; }
    std::string BodyName(int body) const { return body < 0 ? std::string("the ground") : "'" + s.bodies[size_t(body)] + "'"; }
    int Joint(const Json& name) const
    {
        const auto& joints = s.machine.mechanism.joints;
        for (size_t i = 0; i < joints.size(); ++i)
            if (name.is_string() && joints[i].name == name.get<std::string>())
                return int(i);
        return -1;
    }
    int NeedJoint(const doc::ElementInfo& e, const char* field)
    {
        const Json& name = e.definition.contains(field) ? e.definition[field] : Json();
        const int j = Joint(name);
        if (j < 0)
            Error(std::format("The {} '{}' names the joint '{}' in '{}', and there is no such joint.", e.type, e.name, name.is_string() ? name.get<std::string>() : name.dump(),
                              field),
                  std::format("Name an existing joint (aiad_joints_list) in '{}', or define it (aiad_joint_define).", field));
        return j;
    }
    double Number(const doc::ElementInfo& e, const char* field, double fallback)
    {
        if (!e.definition.contains(field))
            return fallback;
        double v = fallback;
        std::string err;
        if (!d.EvaluateNumber(e.definition[field], v, err))
            Error(std::format("The {} '{}': '{}' cannot be evaluated: {}.", e.type, e.name, field, err), "Fix the formula (aiad_element_define).");
        return v;
    }
    Vec3 Vector(const doc::ElementInfo& e, const char* field)
    {
        Vec3 v;
        std::string err;
        if (!d.EvaluateVector(e.definition.value(field, Json()), v, err))
            Error(std::format("The {} '{}': '{}' cannot be evaluated: {}.", e.type, e.name, field, err), "Fix it (aiad_element_define).");
        return v;
    }
    int BodyField(const doc::ElementInfo& e, const char* field)
    {
        const std::string name = e.definition.value(field, "");
        if (name == "ground")
            return -1;
        const int b = Body(name);
        if (b < 0)
            Error(std::format("The {} '{}' names the body '{}' in '{}', which is not a rigid body of the mechanism.", e.type, e.name, name, field),
                  std::format("Name a body with parts (or 'ground') in '{}'.", field));
        return b;
    }
    // Which side of a joint a body is on: +1 body0, -1 body1, 0 neither.
    int Side(int joint, int body) const
    {
        const sim::mb::Joint& j = s.machine.mechanism.joints[size_t(joint)];
        return j.body0 == body ? 1 : j.body1 == body ? -1 : 0;
    }
};

// ---- Couplings: gears, belts, racks -----------------------------------------------------------------------

struct WheelOn
{
    const doc::PartInfo* part = nullptr;
    doc::Wheel wheel;
    Vec3 centre, axis;   // model frame
    int side = 0;        // the wheel turns with +1 / -1 times the joint coordinate
};

bool ReadWheel(Reader& r, const doc::ElementInfo& e, const char* partField, int joint, WheelOn& out)
{
    out.part = r.NeedPart(e, partField);
    if (!out.part || joint < 0)
        return false;
    std::string err;
    if (!doc::FindWheel(out.part->features, r.d.Lookup(), out.wheel, err))
    {
        r.Error(std::format("'{}' ({} of the {} '{}') is not a drawn wheel: {}.", out.part->name, partField, e.type, e.name, err),
                "Draw it as an extruded gear (sketch tool gear), sprocket or pulley (circle).");
        return false;
    }
    const Vec3 o = r.d.BodyOrigin(out.part->body);
    out.centre = Add(out.wheel.centre, o);
    out.axis = out.wheel.axis;
    const sim::mb::Joint& j = r.s.machine.mechanism.joints[size_t(joint)];
    out.side = r.Side(joint, r.Body(out.part->body));
    if (out.side == 0)
    {
        r.Error(std::format("'{}' is in the body '{}', but the joint '{}' turns {} relative to {}: the wheel is not on that shaft.", out.part->name, out.part->body,
                            j.name, r.BodyName(j.body0), r.BodyName(j.body1)),
                std::format("Use the joint the body '{}' turns on, or move the part to the right body (aiad_part_update).", out.part->body));
        return false;
    }
    if (!Rotational(j.type))
    {
        r.Error(std::format("The joint '{}' of the {} '{}' does not turn ({}): a wheel turns on a revolute or cylindrical joint.", j.name, e.type, e.name,
                            j.type == JointType::Prismatic ? "prismatic" : "no rotation axis"),
                "Mount the wheel's body on a revolute joint.");
        return false;
    }
    const Vec3 ja = Unit(FromArr(j.axis));
    const double offAxis = Norm(Radial(out.centre, FromArr(j.point), ja));
    if (Norm(Cross(ja, out.axis)) > 0.01)
        r.Error(std::format("'{}' is drawn with its axis along {} but its joint '{}' turns about {}.", out.part->name, Mm(Mul(out.axis, 1e-3)), j.name,
                            Mm(Mul(ja, 1e-3))),
                "Draw the wheel in the plane perpendicular to the joint axis, or fix the joint axis.");
    else if (offAxis > 0.0005)
        r.Error(std::format("'{}' is not centred on its joint '{}': its centre {} is {:.2f} mm from the axis.", out.part->name, j.name, Mm(out.centre),
                            offAxis * 1e3),
                std::format("Put the joint on the wheel's centre (point {} with aiad_joint_define), or redraw the wheel.", Mm(out.centre)));
    out.axis = Mul(out.axis, Dot(out.axis, ja) < 0 ? -1.0 : 1.0);
    return true;
}

void ReadCoupling(Reader& r, const doc::ElementInfo& e)
{
    const int j0 = r.NeedJoint(e, "joint0"), j1 = r.NeedJoint(e, "joint1");
    WheelOn a;
    if (!ReadWheel(r, e, "part0", j0, a) || j1 < 0)
        return;
    const auto& joints = r.s.machine.mechanism.joints;
    const sim::mb::Joint& J0 = joints[size_t(j0)];
    const sim::mb::Joint& J1 = joints[size_t(j1)];
    const Vec3 a0 = Unit(FromArr(J0.axis)), a1 = Unit(FromArr(J1.axis));
    if (e.type == "rack")
    {
        if (J1.type != JointType::Prismatic)
        {
            r.Error(std::format("The rack '{}' slides on '{}', which is not a prismatic joint.", e.name, J1.name), "Mount the rack's body on a prismatic joint.");
            return;
        }
        // The pinion's pitch point toward the rack, moving along the slide.
        const doc::PartInfo* rack = e.definition.contains("part1") ? r.NeedPart(e, "part1") : nullptr;
        Vec3 towards = FromArr(J1.point);
        if (rack && !r.Points(*rack).empty())
        {
            Vec3 sum;
            for (const Vec3& p : r.Points(*rack))
                sum = Add(sum, p);
            towards = Mul(sum, 1.0 / double(r.Points(*rack).size()));
        }
        const Vec3 dir = Unit(Radial(towards, a.centre, a.axis));
        const Vec3 s1 = Unit(FromArr(J1.axis));
        const double along = Dot(Cross(a.axis, dir), s1);
        if (std::abs(along) < 0.5)
        {
            r.Error(std::format("The rack '{}' slides along {} but the pinion '{}' pushes its teeth along {}: they do not mesh.", e.name, Mm(Mul(s1, 1e-3)),
                                a.part->name, Mm(Mul(Cross(a.axis, dir), 1e-3))),
                    "Make the rack's prismatic joint tangent to the pinion's pitch circle.");
            return;
        }
        const int rackSide = rack ? r.Side(j1, r.Body(rack->body)) : 1;
        const double ratio = a.side * (rackSide == 0 ? 1 : rackSide) * a.wheel.pitchRadius * along * Dot(a.axis, a0);
        r.s.machine.mechanism.couplings.push_back({e.name, j0, j1, ratio});
        r.s.measured.push_back(std::format("Rack {}: '{}' (pitch radius {:.2f} mm) drives '{}' by {:.3f} mm per radian.", e.name, a.part->name,
                                           a.wheel.pitchRadius * 1e3, J1.name, ratio * 1e3));
        return;
    }
    WheelOn b;
    if (!ReadWheel(r, e, "part1", j1, b))
        return;
    if (Norm(Cross(a.axis, b.axis)) > 0.01)
    {
        r.Error(std::format("'{}' and '{}' turn about axes that are not parallel: crossed and bevel gears are not simulated yet.", a.part->name, b.part->name),
                "Draw the two wheels on parallel axes.");
        return;
    }
    // Wheels on a common carrier: the joints turn relative to the same body.
    const int carrier0 = a.side > 0 ? J0.body1 : J0.body0, carrier1 = b.side > 0 ? J1.body1 : J1.body0;
    auto ground = [&](int i) { return i < 0 || !r.Moving(i); };
    if (carrier0 != carrier1 && !(ground(carrier0) && ground(carrier1)))
        r.Error(std::format("'{}' turns relative to {} and '{}' relative to {}: the two wheels' shafts must be held by the same body.", a.part->name,
                            r.BodyName(carrier0), b.part->name, r.BodyName(carrier1)),
                "Mount both joints on the same body (the frame, or the carrier of a planetary train).");
    const double apart = Dot(Sub(b.centre, a.centre), a.axis);
    const double distance = Norm(Radial(b.centre, a.centre, a.axis));
    if (std::abs(apart) > (a.wheel.width + b.wheel.width) / 2.0)
    {
        r.Error(std::format("'{}' and '{}' are not in the same plane: {:.1f} mm apart along their axes, so they do not {}.", a.part->name, b.part->name,
                            std::abs(apart) * 1e3, e.type == "gear" ? "mesh" : "line up"),
                "Move one of them along its axis (its features) into the other's plane.");
        return;
    }
    const bool internal = e.definition.value("internal", false);
    const bool teeth = a.wheel.teeth > 0 && b.wheel.teeth > 0;
    if (e.type == "gear")
    {
        if (!teeth)
        {
            r.Error(std::format("The gear '{}' needs teeth on both wheels ('{}': {}, '{}': {}).", e.name, a.part->name, a.wheel.teeth, b.part->name, b.wheel.teeth),
                    "Draw them with the gear or sprocket sketch tool (a smooth wheel is a friction drive: use a belt element for pulleys).");
            return;
        }
        const double pitchA = 2.0 * kPi * a.wheel.pitchRadius / a.wheel.teeth, pitchB = 2.0 * kPi * b.wheel.pitchRadius / b.wheel.teeth;
        if (std::abs(pitchA - pitchB) > 0.03 * (pitchA + pitchB) / 2.0)
            r.Error(std::format("'{}' and '{}' have different tooth pitches ({:.2f} and {:.2f} mm on the pitch circle): their teeth cannot mesh.", a.part->name,
                                b.part->name, pitchA * 1e3, pitchB * 1e3),
                    "Draw both with the same module (pitch radius / teeth).");
        if (internal)
        {
            const double expected = std::abs(b.wheel.pitchRadius - a.wheel.pitchRadius);
            if (std::abs(distance - expected) > 0.5 * std::max(a.wheel.module, b.wheel.module))
                r.Error(std::format("The internal gear '{}': centre distance {:.2f} mm, the pitch circles need {:.2f} mm.", e.name, distance * 1e3, expected * 1e3),
                        "Move one of the joints (and its wheel) to that distance.");
        }
        else
        {
            const double interference = std::max(a.wheel.tipRadius + b.wheel.rootRadius, a.wheel.rootRadius + b.wheel.tipRadius) - distance;
            const double depth = std::min(a.wheel.tipRadius - a.wheel.rootRadius, b.wheel.tipRadius - b.wheel.rootRadius);
            const double engagement = a.wheel.tipRadius + b.wheel.tipRadius - distance;
            const double needed = a.wheel.pitchRadius + b.wheel.pitchRadius;
            if (interference > 0.0001)
                r.Error(std::format("The teeth of '{}' and '{}' dig {:.2f} mm into each other: their centres are {:.2f} mm apart, the pitch circles need "
                                    "{:.2f} mm.",
                                    a.part->name, b.part->name, interference * 1e3, distance * 1e3, needed * 1e3),
                        "Move one of the shafts (joint and wheel) apart to the sum of the pitch radii, or redraw the wheels.");
            else if (engagement < 0.3 * depth)
                r.Error(std::format("'{}' and '{}' do not mesh: their centres are {:.2f} mm apart, their tips reach {:.2f} mm.", a.part->name, b.part->name,
                                    distance * 1e3, (a.wheel.tipRadius + b.wheel.tipRadius) * 1e3),
                        std::format("Move the shafts to {:.2f} mm apart (the sum of the pitch radii), or redraw the wheels.", needed * 1e3));
        }
    }
    else
    {
        const doc::PartInfo* belt = e.definition.contains("belt") ? r.NeedPart(e, "belt") : nullptr;
        if (!belt)
            r.Error(std::format("No belt or chain is drawn for the drive '{}' between '{}' and '{}'.", e.name, a.part->name, b.part->name),
                    "Draw the belt or chain around both wheels and name it in the element (belt: part).");
        else
        {
            Box box;
            for (const Vec3& p : r.Points(*belt))
                box.Add(p, p);
            if (box.Empty() || !box.Contains(a.centre, 0.0) || !box.Contains(b.centre, 0.0))
                r.Error(std::format("The belt '{}' does not pass around both wheels (centres {} and {}).", belt->name, Mm(a.centre), Mm(b.centre)),
                        "Redraw the belt around both wheels.");
        }
    }
    // rate(joint1) = ratio rate(joint0): the wheels' speed ratio (teeth, else pitch radii), opposite for an
    // external mesh, the same for belts and internal gears; the joints' sides and axes.
    const double speed = teeth ? double(a.wheel.teeth) / b.wheel.teeth : a.wheel.pitchRadius / b.wheel.pitchRadius;
    const double direction = e.type == "gear" && !internal ? -1.0 : 1.0;
    const double ratio = direction * speed * a.side * b.side * Dot(a.axis, a0) * Dot(b.axis, a1) * Dot(a.axis, b.axis);
    r.s.machine.mechanism.couplings.push_back({e.name, j0, j1, ratio});
    r.s.measured.push_back(std::format("{} {}: '{}' ({} teeth, pitch radius {:.2f} mm) -> '{}' ({} teeth, {:.2f} mm), centres {:.2f} mm apart: '{}' turns {:+.4f} "
                                       "x '{}'.",
                                       e.type == "gear" ? "Gear" : "Belt", e.name, a.part->name, a.wheel.teeth, a.wheel.pitchRadius * 1e3, b.part->name,
                                       b.wheel.teeth, b.wheel.pitchRadius * 1e3, distance * 1e3, J1.name, ratio, J0.name));
}

// ---- Contacts: a drawn profile on a flat face ---------------------------------------------------------------

void ReadContact(Reader& r, const doc::ElementInfo& e)
{
    const doc::PartInfo* part = r.NeedPart(e, "part");
    if (!part)
        return;
    doc::ExtrudedProfile profile;
    std::string err;
    if (!doc::FindExtrudedProfile(part->features, r.d.Lookup(), profile, err))
    {
        r.Error(std::format("'{}' (the profile of the contact '{}') has no extruded profile: {}.", part->name, e.name, err),
                "A contact needs the drawn profile of a cam lobe or a pad: an extruded sketch.");
        return;
    }
    sim::mb::Contact c;
    c.name = e.name;
    c.body = r.Body(part->body);
    c.follower = r.BodyField(e, "body");
    const Vec3 o = r.d.BodyOrigin(part->body);
    const Vec3 point = r.Vector(e, "point"), normal = Unit(r.Vector(e, "normal"));
    c.point = Arr(point);
    c.normal = Arr(normal);
    c.radius = r.Number(e, "radius", 0.0);
    c.stiffness = r.Number(e, "stiffness", c.stiffness);
    for (const Vec3& p : profile.points)
        c.profile.push_back(Arr(Add(p, o)));
    const Vec3 centre = Add(profile.centre, o), axis = profile.axis;
    if (c.body < 0)
    {
        r.Error(std::format("The profile '{}' of the contact '{}' is in '{}', a body that only holds a spring.", part->name, e.name, part->body),
                "Put the profile in the rigid body that carries it.");
        return;
    }
    if (!r.Moving(c.body) && !r.Moving(c.follower))
    {
        r.Error(std::format("In the contact '{}' neither '{}' nor {} moves.", e.name, part->body, r.BodyName(c.follower)), "Mount them on joints.");
        return;
    }
    if (std::abs(Dot(normal, axis)) > 0.05)
        r.Error(std::format("The face of {} in the contact '{}' is not parallel to the axis of the profile '{}': the profile would touch it on an edge.",
                            r.BodyName(c.follower), e.name, part->name),
                "Give the contact a normal perpendicular to the profile's extrusion axis.");
    const double beside = std::abs(Dot(Sub(point, centre), axis));
    if (beside > profile.width / 2.0 + 0.0005)
        r.Error(std::format("The face of the contact '{}' (at {}) is {:.1f} mm beside the profile '{}', which is {:.1f} mm wide: they never touch.", e.name,
                            Mm(point), (beside - profile.width / 2.0) * 1e3, part->name, profile.width * 1e3),
                "Move the face (point) over the profile, or move the part along its axis.");
    // Over a turn of the profile's joint (a cam on its shaft): how far it reaches into the face.
    auto depthAt = [&](const std::vector<Vec3>& pts) {
        double depth = -1e9;
        for (const Vec3& p : pts)
        {
            const Vec3 dvec = Sub(p, point);
            const double along = Dot(dvec, normal);
            if (Norm(Sub(dvec, Mul(normal, along))) <= c.radius)
                depth = std::max(depth, -along);
        }
        return depth;
    };
    // Only a profile that keeps turning (a cam: its joint is driven by a coupling or a motor) is swept over a
    // turn; an oscillating one (a rocker pad) is checked where it is drawn.
    int turning = -1;
    const auto& joints = r.s.machine.mechanism.joints;
    for (size_t j = 0; j < joints.size() && turning < 0; ++j)
    {
        const bool driven = joints[j].motor.mode != sim::mb::Motor::Mode::None ||
                            std::any_of(r.s.machine.mechanism.couplings.begin(), r.s.machine.mechanism.couplings.end(),
                                        [&](const sim::mb::Coupling& k) { return k.joint0 == int(j) || k.joint1 == int(j); });
        if (Rotational(joints[j].type) && r.Side(int(j), c.body) != 0 && driven)
            turning = int(j);
    }
    std::vector<Vec3> pts;
    for (const auto& p : c.profile)
        pts.push_back(FromArr(p));
    if (turning < 0)
    {
        const double depth = depthAt(pts);
        if (depth < -1e8)
            r.Error(std::format("The face of the contact '{}' (radius {:.1f} mm at {}) is not under the profile '{}'.", e.name, c.radius * 1e3, Mm(point), part->name),
                    "Put the face's centre (point) under the profile, facing it (normal toward the profile).");
        else if (depth > 0.00005)
            r.Error(std::format("The profile '{}' cuts {:.2f} mm into the face of {} at rest.", part->name, depth * 1e3, r.BodyName(c.follower)),
                    "Move one of them apart until they just touch (or leave a small lash).");
        else
        {
            r.s.measured.push_back(std::format("Contact {}: '{}' on {}, {:.2f} mm apart at rest.", e.name, part->name, r.BodyName(c.follower), -depth * 1e3));
            if (-depth > 0.001)
                r.Note(std::format("The lash of the contact '{}' is {:.2f} mm: the follower moves late and less.", e.name, -depth * 1e3),
                       "Move the follower closer (0.1 to 0.3 mm lash is usual for valves).");
        }
        r.s.machine.mechanism.contacts.push_back(c);
        return;
    }
    const sim::mb::Joint& J = joints[size_t(turning)];
    const Vec3 ja = Unit(FromArr(J.axis)), jp = FromArr(J.point);
    double reach = -1e9, base = 1e9;
    for (int k = 0; k < 360; ++k)
    {
        const double t = k / kDeg, cs = std::cos(t), sn = std::sin(t);
        std::vector<Vec3> turned;
        turned.reserve(pts.size());
        for (const Vec3& p : pts)
        {
            const Vec3 v = Sub(p, jp);
            const Vec3 rot = Add(Add(Mul(v, cs), Mul(Cross(ja, v), sn)), Mul(ja, Dot(ja, v) * (1 - cs)));
            turned.push_back(Add(jp, rot));
        }
        const double depth = depthAt(turned);
        reach = std::max(reach, depth);
        if (depth > -1e8)
            base = std::min(base, depth);
    }
    if (reach < -1e8)
        r.Error(std::format("The face of the contact '{}' (radius {:.1f} mm at {}) is not under the profile '{}' at any angle of '{}'.", e.name, c.radius * 1e3,
                            Mm(point), part->name, J.name),
                "Put the face's centre (point) on the line the profile sweeps, facing it (normal toward the profile).");
    else if (reach <= 0.0)
        r.Error(std::format("The profile '{}' never reaches the face of {} in the contact '{}': over a whole turn of '{}' it stays {:.2f} mm short, so it "
                            "moves nothing.",
                            part->name, r.BodyName(c.follower), e.name, J.name, -reach * 1e3),
                std::format("Move the follower (or the profile's shaft) {:.2f} mm toward each other, or redraw the profile with more lift.", -reach * 1e3 + 0.1e-3));
    else if (base > 0.00005)
        r.Error(std::format("The profile '{}' cuts {:.2f} mm into the face of {} even at its lowest point: the follower is never released (a valve would "
                            "never close).",
                            part->name, base * 1e3, r.BodyName(c.follower)),
                std::format("Move them {:.2f} mm apart (leave a small lash), or redraw the profile.", base * 1e3 + 0.1e-3));
    else
    {
        r.s.measured.push_back(std::format("Contact {}: '{}' lifts {} by {:.2f} mm over a turn of '{}' (lash {:.2f} mm).", e.name, part->name,
                                           r.BodyName(c.follower), (reach - std::max(base, 0.0)) * 1e3, J.name, std::max(-base, 0.0) * 1e3));
        if (-base > 0.001)
            r.Note(std::format("The lash of the contact '{}' is {:.2f} mm: the follower moves late and less than the profile's lift.", e.name, -base * 1e3),
                   "Move the follower closer (0.1 to 0.3 mm lash is usual for valves).");
    }
    r.s.machine.mechanism.contacts.push_back(c);
}

// ---- Springs ------------------------------------------------------------------------------------------------------

void ReadSpring(Reader& r, const doc::ElementInfo& e, const std::set<std::string>& springBodies)
{
    const doc::PartInfo* part = r.NeedPart(e, "part");
    if (!part)
        return;
    doc::Helix helix;
    std::string err;
    if (!doc::FindHelix(part->features, r.d.Lookup(), helix, err))
    {
        r.Error(std::format("'{}' (the spring '{}') is not a drawn coil spring: {}.", part->name, e.name, err), "Draw it with a helix_pipe feature.");
        return;
    }
    const std::optional<doc::Material> material = r.d.FindMaterial(part->material);
    const double G = material ? material->shearModulus : 0.0;
    if (G <= 0.0)
    {
        r.Error(std::format("The material '{}' of the spring '{}' has no shear modulus, so its stiffness is unknown.", part->material, part->name),
                std::format("Give it one: aiad_material_define '{}' with shear_modulus (spring steel 79.3e9 Pa).", part->material));
        return;
    }
    sim::mb::Spring s;
    s.name = e.name;
    s.body0 = r.BodyField(e, "body0");
    s.body1 = r.BodyField(e, "body1");
    const Vec3 p0 = r.Vector(e, "point0"), p1 = r.Vector(e, "point1");
    s.point0 = Arr(p0);
    s.point1 = Arr(p1);
    const double turns = r.Number(e, "active_turns", std::max(1.0, helix.turns - 2.0));   // closed, ground ends
    const double wire = 2.0 * helix.wireRadius, coil = 2.0 * helix.coilRadius;
    s.stiffness = G * std::pow(wire, 4) / (8.0 * std::pow(coil, 3) * turns);
    s.freeLength = r.Number(e, "free_length", 0.0);
    s.damping = r.Number(e, "damping", 0.0);
    const double installed = Norm(Sub(p1, p0));
    const Vec3 o = r.d.BodyOrigin(part->body), base = Add(helix.base, o);
    if (std::abs(installed - helix.height) > 0.001)
        r.Error(std::format("The spring '{}' is drawn {:.1f} mm long but its ends (point0, point1) are {:.1f} mm apart.", part->name, helix.height * 1e3,
                            installed * 1e3),
                "Put the ends on the spring's seats, or redraw the spring to the installed length.");
    else if (Norm(Radial(p0, base, helix.axis)) > 0.001 || Norm(Radial(p1, base, helix.axis)) > 0.001)
        r.Error(std::format("The ends of the spring '{}' ({} and {}) are not on its axis.", part->name, Mm(p0), Mm(p1)), "Put point0 and point1 on the coil's axis.");
    if (s.body0 == s.body1)
        r.Error(std::format("Both ends of the spring '{}' are on {}: it does nothing.", e.name, r.BodyName(s.body0)), "Attach its ends to the two bodies it pushes apart.");
    if (!springBodies.contains(part->body))
        r.Error(std::format("The spring '{}' is in the body '{}', which also holds other parts: the view stretches a spring's body with it.", part->name, part->body),
                std::format("Give the spring a body of its own (aiad_body_define, then aiad_part_update body)."));
    const double preload = s.stiffness * (s.freeLength - installed), solid = wire * (helix.turns + 1.0);
    if (s.freeLength <= installed)
        r.Note(std::format("The spring '{}' is not compressed when installed (free length {:.1f} mm, installed {:.1f} mm): no preload.", e.name, s.freeLength * 1e3,
                           installed * 1e3),
               "Give it a free length longer than the installed length.");
    r.s.measured.push_back(std::format("Spring {}: wire {:.2f} mm, coil {:.1f} mm, {:.1f} active turns, '{}' (G {:.1f} GPa): {:.1f} N/mm, preload {:.0f} N "
                                       "at {:.1f} mm; coil bind after {:.1f} mm more.",
                                       e.name, wire * 1e3, coil * 1e3, turns, part->material, G * 1e-9, s.stiffness * 1e-3, preload, installed * 1e3,
                                       (installed - solid) * 1e3));
    r.s.machine.mechanism.springs.push_back(s);
    r.s.springBodies.push_back({part->body, int(r.s.machine.mechanism.springs.size()) - 1});
}

// ---- Spiral springs and touching outlines -----------------------------------------------------------------------------

// Extent of a part along a direction (model frame, from its tessellation).
void Extent(Reader& r, const doc::PartInfo& part, const Vec3& n, double& lo, double& hi)
{
    lo = 1e300, hi = -1e300;
    for (const Vec3& p : r.Points(part))
        lo = std::min(lo, Dot(p, n)), hi = std::max(hi, Dot(p, n));
}

// The part's outline on the plane {x : x.n = height} (model frame).
std::vector<std::vector<Vec3>> OutlineAt(Reader& r, const doc::PartInfo& part, const Vec3& n, double height)
{
    const Vec3 o = r.d.BodyOrigin(part.body);
    std::vector<std::vector<Vec3>> loops = r.d.SectionOutline(part.path, Sub(Mul(n, height), o), n);
    for (auto& loop : loops)
        for (Vec3& p : loop)
            p = Add(p, o);
    return loops;
}

// In-plane coordinates (u, v) for a normal.
void PlaneAxes(const Vec3& n, Vec3& u, Vec3& v)
{
    u = Unit(Cross(n, std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0}));
    v = Cross(n, u);
}

void ReadTorsion(Reader& r, const doc::ElementInfo& e)
{
    const int j = r.NeedJoint(e, "joint");
    const doc::PartInfo* part = r.NeedPart(e, "part");
    if (j < 0 || !part)
        return;
    const sim::mb::Joint& J = r.s.machine.mechanism.joints[size_t(j)];
    const int body = r.Body(part->body);
    const bool grounded = body >= 0 && !r.Moving(body) && (!r.Moving(J.body0) || !r.Moving(J.body1));
    if (body < 0 || (r.Side(j, body) == 0 && !grounded))
    {
        r.Error(std::format("The spiral spring '{}' ('{}', in '{}') is not on either body of the joint '{}', which turns {} relative to {}.", e.name, part->name,
                            part->body, J.name, r.BodyName(J.body0), r.BodyName(J.body1)),
                "Name the joint it winds around, or draw it in one of that joint's bodies (aiad_part_update body).");
        return;
    }
    const Vec3 axis = Unit(FromArr(J.axis)), centre = FromArr(J.point);
    double lo, hi;
    Extent(r, *part, axis, lo, hi);
    const double height = hi - lo;
    const std::vector<std::vector<Vec3>> loops = OutlineAt(r, *part, axis, (lo + hi) / 2.0);
    // The strip: its length is half the outline's perimeter, its thickness the section's area over that.
    double perimeter = 0.0, area = 0.0, winding = 0.0;
    Vec3 u, v;
    PlaneAxes(axis, u, v);
    for (const auto& loop : loops)
    {
        double turned = 0.0, low = 0.0, high = 0.0, previous = 0.0;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            const Vec3& a = loop[i];
            const Vec3& b = loop[(i + 1) % loop.size()];
            perimeter += Norm(Sub(b, a));
            area += Dot(Cross(Sub(a, centre), Sub(b, centre)), axis) / 2.0;
            const Vec3 d = Sub(a, centre);
            const double angle = std::atan2(Dot(d, v), Dot(d, u));
            if (i > 0)
                turned += std::remainder(angle - previous, 2.0 * std::numbers::pi);
            previous = angle;
            low = std::min(low, turned), high = std::max(high, turned);
        }
        winding = std::max(winding, high - low);
    }
    double length = perimeter / 2.0;
    const double thickness = length > 0.0 ? std::abs(area) / length : 0.0;
    if (loops.empty() || thickness <= 0.0)
    {
        r.Error(std::format("'{}' (the spiral spring '{}') has no section across the axis of '{}' at its mid-height.", part->name, e.name, J.name),
                "Draw the spring as a strip wound around the joint's axis (sketch tool spiral, extruded along the axis).");
        return;
    }
    if (thickness > 0.05 * length)
    {
        r.Error(std::format("'{}' is not a spiral strip: its section is {:.2f} mm long and {:.3f} mm thick.", part->name, length * 1e3, thickness * 1e3),
                "A spiral spring is a long thin strip (sketch tool spiral).");
        return;
    }
    if (winding < 1.8 * std::numbers::pi)
    {
        r.Error(std::format("'{}' does not wind around the axis of '{}': its outline turns {:.0f} deg around it.", part->name, J.name,
                            winding * 180.0 / std::numbers::pi),
                std::format("Name the joint whose staff or arbor it is coiled around, or put the joint on the spiral's centre."));
        return;
    }
    const std::optional<doc::Material> material = r.d.FindMaterial(part->material);
    const double E = material ? material->elasticModulus : 0.0;
    if (E <= 0.0)
    {
        r.Error(std::format("The material '{}' of the spiral spring '{}' has no elastic modulus, so its stiffness is unknown.", part->material, part->name),
                std::format("Give it one: aiad_material_define '{}' with elastic_modulus (steel 200e9 Pa, Nivarox 190e9 Pa).", part->material));
        return;
    }
    // A holder (a watch regulator's index pins): the strip works only up to where it holds it; the rest, to the outer end
    // (the stud), does not bend. The holder is where its body is set (an adjuster's start), so moving it changes the rate.
    std::string held;
    if (const std::string holderName = e.definition.value("holder", ""); !holderName.empty())
    {
        const doc::PartInfo* holder = r.NeedPart(e, "holder");
        if (!holder)
            return;
        Vec3 c{0, 0, 0};
        const std::vector<Vec3>& pts = r.Points(*holder);
        for (const Vec3& p : pts)
            c = Add(c, Mul(p, 1.0 / double(std::max<size_t>(pts.size(), 1))));
        // Where its body is set: turned (slid) by the start of the joint that moves it.
        const int hb = r.Body(holder->body);
        for (const sim::mb::Joint& HJ : r.s.machine.mechanism.joints)
            if (hb >= 0 && HJ.start != 0.0 && (HJ.body0 == hb || HJ.body1 == hb) && r.Moving(hb))
            {
                const double turn = HJ.body0 == hb ? HJ.start : -HJ.start;
                const Vec3 ha = Unit(FromArr(HJ.axis)), hp = FromArr(HJ.point);
                if (HJ.type == sim::mb::JointType::Prismatic)
                    c = Add(c, Mul(ha, turn));
                else
                {
                    const Vec3 d = Sub(c, hp), along = Mul(ha, Dot(d, ha)), across = Sub(d, along);
                    c = Add(hp, Add(along, Add(Mul(across, std::cos(turn)), Mul(Cross(ha, across), std::sin(turn)))));
                }
                break;
            }
        // The strip's outer end and the way it winds outward, from its outline about the axis.
        double outerR = -1.0, innerR = 1e300, outerTurn = 0.0, innerTurn = 0.0, outerAngle = 0.0;
        for (const auto& loop : loops)
        {
            double turned = 0.0, previous = 0.0;
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const Vec3 d = Sub(loop[i], centre);
                const double angle = std::atan2(Dot(d, v), Dot(d, u)), radius = Norm(Radial(loop[i], centre, axis));
                if (i > 0)
                    turned += std::remainder(angle - previous, 2.0 * std::numbers::pi);
                previous = angle;
                if (radius > outerR)
                    outerR = radius, outerTurn = turned, outerAngle = angle;
                if (radius < innerR)
                    innerR = radius, innerTurn = turned;
            }
        }
        const double outward = outerTurn >= innerTurn ? 1.0 : -1.0;
        const Vec3 hd = Radial(c, centre, axis);
        const double holdR = Norm(hd), holdAngle = std::atan2(Dot(hd, v), Dot(hd, u));
        // From the holder outward to the end, the way the strip winds out.
        const double slack = std::fmod(std::fmod(outward * (outerAngle - holdAngle), 2.0 * std::numbers::pi) + 2.0 * std::numbers::pi, 2.0 * std::numbers::pi);
        const double cut = holdR * slack;
        if (holdR < innerR || holdR > outerR * 1.2 || cut >= 0.5 * length)
        {
            r.Error(std::format("The holder '{}' of the spiral spring '{}' is not on its outer coil ({:.2f} mm from the axis; the strip spans {:.2f} .. {:.2f} mm).",
                                holder->name, e.name, holdR * 1e3, innerR * 1e3, outerR * 1e3),
                    "Name as holder the part that holds the strip near its outer end (a regulator's index pins).");
            return;
        }
        length -= cut;
        held = std::format("; held by '{}' {:.2f} mm before its end, so {:.1f} mm of it work", holder->name, cut * 1e3, length * 1e3);
    }
    sim::mb::TorsionSpring t;
    t.name = e.name;
    t.joint = j;
    t.stiffness = E * height * std::pow(thickness, 3) / (12.0 * length);
    t.free = r.Number(e, "free", 0.0);
    t.damping = r.Number(e, "damping", 0.0);
    std::string text = std::format("Spiral spring {}: '{}', strip {:.1f} mm long, {:.1f} um thick, {:.2f} mm high, E {:.0f} GPa: {:.4g} N mm/rad on '{}'", e.name,
                                   part->name, perimeter / 2.0 * 1e3, thickness * 1e6, height * 1e3, E * 1e-9, t.stiffness * 1e3, J.name);
    text += held;
    if (t.free != 0.0)
        text += std::format(", wound {:.2f} turns: {:.4g} N mm at rest", t.free / (2.0 * std::numbers::pi), t.stiffness * t.free * 1e3);
    // What it oscillates with alone (a balance): the moving side's inertia about the axis.
    const int moving = r.Moving(J.body0) && !r.Moving(J.body1) ? J.body0 : !r.Moving(J.body0) && r.Moving(J.body1) ? J.body1 : -1;
    if (moving >= 0)
    {
        const sim::mb::Body& b = r.s.machine.mechanism.bodies[size_t(moving)];
        const double t3[3][3] = {{b.inertia[0], b.inertia[5], b.inertia[4]}, {b.inertia[5], b.inertia[1], b.inertia[3]}, {b.inertia[4], b.inertia[3], b.inertia[2]}};
        const double a[3] = {axis.x, axis.y, axis.z};
        double I = 0.0;
        for (int p = 0; p < 3; ++p)
            for (int q = 0; q < 3; ++q)
                I += a[p] * t3[p][q] * a[q];
        const Vec3 off = Radial(FromArr(b.centroid), centre, axis);
        I += b.mass * Dot(off, off);
        const double f = std::sqrt(t.stiffness / I) / (2.0 * std::numbers::pi);
        text += std::format("; with {} ({:.3g} kg m^2) it swings at {:.3f} Hz ({:.0f} beats per hour)", r.BodyName(moving), I, f, 2.0 * f * 3600.0);
    }
    r.s.measured.push_back(text + ".");
    r.s.machine.mechanism.torsions.push_back(t);
}

void ReadTouch(Reader& r, const doc::ElementInfo& e)
{
    const doc::PartInfo* p0 = r.NeedPart(e, "part0");
    const doc::PartInfo* p1 = r.NeedPart(e, "part1");
    if (!p0 || !p1)
        return;
    const int b0 = r.Body(p0->body), b1 = r.Body(p1->body);
    if (b0 < 0 || b1 < 0)
    {
        r.Error(std::format("A part of the touch '{}' is in a body that only holds a spring.", e.name), "Name rigid parts.");
        return;
    }
    if (b0 == b1)
    {
        r.Error(std::format("'{}' and '{}' (the touch '{}') are both in '{}': they never move relative to each other.", p0->name, p1->name, e.name, p0->body),
                "Name parts of the two bodies that touch.");
        return;
    }
    if (!r.Moving(b0) && !r.Moving(b1))
    {
        r.Error(std::format("In the touch '{}' neither '{}' nor '{}' moves.", e.name, p0->body, p1->body), "Mount them on joints.");
        return;
    }
    // The plane they move in: the axis their bodies turn about.
    Vec3 n;
    bool planar = false;
    for (const int b : {b0, b1})
    {
        if (!r.Moving(b))
            continue;
        for (const sim::mb::Joint& j : r.s.machine.mechanism.joints)
            if (Rotational(j.type) && (j.body0 == b || j.body1 == b))
            {
                const Vec3 a = Unit(FromArr(j.axis));
                if (!planar)
                    n = a, planar = true;
                else if (Norm(Cross(n, a)) > 0.01)
                {
                    r.Error(std::format("'{}' and '{}' (the touch '{}') turn about different axes ({} and {}): their outlines do not move in one plane.",
                                        p0->body, p1->body, e.name, Mm(Mul(n, 1e-3)), Mm(Mul(a, 1e-3))),
                            "A touch is planar: the two bodies must turn about parallel axes.");
                    return;
                }
                break;
            }
    }
    if (!planar)
    {
        r.Error(std::format("Neither body of the touch '{}' turns on a revolute joint: the plane the outlines move in is unknown.", e.name),
                "Mount the moving part on a revolute (or cylindrical) joint.");
        return;
    }
    double lo0, hi0, lo1, hi1;
    Extent(r, *p0, n, lo0, hi0);
    Extent(r, *p1, n, lo1, hi1);
    const double lo = std::max(lo0, lo1), hi = std::min(hi0, hi1);
    if (hi - lo <= 1e-6)
    {
        r.Error(std::format("'{}' spans {:.3f} .. {:.3f} mm and '{}' {:.3f} .. {:.3f} mm along their axis: at different heights, they never touch.", p0->name,
                            lo0 * 1e3, hi0 * 1e3, p1->name, lo1 * 1e3, hi1 * 1e3),
                "Name the parts that are at the same height, or move one of them along the axis.");
        return;
    }
    sim::mb::Touch t;
    t.name = e.name;
    t.body0 = b0;
    t.body1 = b1;
    t.normal = Arr(n);
    t.stiffness = r.Number(e, "stiffness", 0.0);
    t.damping = r.Number(e, "damping", -1.0);
    const double height = e.definition.contains("at") ? r.Number(e, "at", 0.0) : (lo + hi) / 2.0;
    if (height < lo - 1e-9 || height > hi + 1e-9)
    {
        r.Error(std::format("The touch '{}' is cut at {:.3f} mm, outside the heights both parts share ({:.3f} .. {:.3f} mm).", e.name, height * 1e3, lo * 1e3, hi * 1e3),
                "Give 'at' a height inside that range, or leave it out (the middle of it).");
        return;
    }
    std::vector<std::vector<Vec3>> o0 = OutlineAt(r, *p0, n, height), o1 = OutlineAt(r, *p1, n, height);
    if (o0.empty() || o1.empty())
    {
        r.Error(std::format("The touch '{}': '{}' has no outline at {:.3f} mm.", e.name, (o0.empty() ? p0 : p1)->name, height * 1e3),
                "Check that the part is a closed solid there.");
        return;
    }
    Vec3 u, v;
    PlaneAxes(n, u, v);
    // A fixed side (a plate with its bankings) is cropped to where the moving side can reach: a square around the
    // moving body's axis, as wide as its outline's farthest point (plus a margin). Clipping each loop to a convex
    // window keeps which points are in the material inside the window.
    for (int side = 0; side < 2; ++side)
    {
        const int still = side == 0 ? b0 : b1, moving = side == 0 ? b1 : b0;
        if (r.Moving(still) || !r.Moving(moving))
            continue;
        const sim::mb::Joint* turn = nullptr;
        for (const sim::mb::Joint& j : r.s.machine.mechanism.joints)
            if (Rotational(j.type) && (j.body0 == moving || j.body1 == moving))
            {
                turn = &j;
                break;
            }
        if (!turn || r.Moving(turn->body0 == moving ? turn->body1 : turn->body0))
            continue;   // its axis moves too: no fixed reach
        const Vec3 c = FromArr(turn->point);
        double reach = 0.0;
        for (const auto& loop : side == 0 ? o1 : o0)
            for (const Vec3& p : loop)
                reach = std::max(reach, Norm(Radial(p, c, n)));
        reach = reach * 1.1 + 0.0002;
        const double cu = Dot(c, u), cv = Dot(c, v);
        std::vector<std::vector<Vec3>> cropped;
        for (const auto& loop : side == 0 ? o0 : o1)
        {
            std::vector<Vec3> poly = loop;
            // Sutherland-Hodgman against the four sides of the square (u, v around c).
            for (int edge = 0; edge < 4 && !poly.empty(); ++edge)
            {
                const Vec3 axis = edge < 2 ? u : v;
                const double limit = (edge < 2 ? cu : cv) + (edge % 2 == 0 ? reach : -reach), sign = edge % 2 == 0 ? 1.0 : -1.0;
                auto inside = [&](const Vec3& p) { return sign * (Dot(p, axis) - limit) <= 0.0; };
                std::vector<Vec3> out;
                for (size_t i = 0; i < poly.size(); ++i)
                {
                    const Vec3& a = poly[i];
                    const Vec3& b = poly[(i + 1) % poly.size()];
                    if (inside(a))
                        out.push_back(a);
                    if (inside(a) != inside(b))
                    {
                        const double da = Dot(a, axis) - limit, db = Dot(b, axis) - limit;
                        out.push_back(Add(a, Mul(Sub(b, a), da / (da - db))));
                    }
                }
                poly = std::move(out);
            }
            if (poly.size() >= 3)
                cropped.push_back(std::move(poly));
        }
        (side == 0 ? o0 : o1) = std::move(cropped);
    }
    // At rest: how far one outline enters the other, or the gap between them (in the plane).
    using Loops = std::vector<std::vector<std::array<double, 2>>>;
    auto flat = [&](const std::vector<std::vector<Vec3>>& loops) {
        Loops out;
        for (const auto& loop : loops)
        {
            auto& l = out.emplace_back();
            for (const Vec3& p : loop)
                l.push_back({Dot(p, u), Dot(p, v)});
        }
        return out;
    };
    const Loops f0 = flat(o0), f1 = flat(o1);
    auto inside = [](const Loops& loops, double x, double y) {
        bool in = false;
        for (const auto& l : loops)
            for (size_t i = 0, k = l.size() - 1; i < l.size(); k = i++)
                if ((l[i][1] > y) != (l[k][1] > y) && x < (l[k][0] - l[i][0]) * (y - l[i][1]) / (l[k][1] - l[i][1]) + l[i][0])
                    in = !in;
        return in;
    };
    auto nearest = [](const Loops& loops, double x, double y) {
        double best = 1e300;
        for (const auto& l : loops)
            for (size_t i = 0; i < l.size(); ++i)
            {
                const auto& a = l[i];
                const auto& b = l[(i + 1) % l.size()];
                const double eu = b[0] - a[0], ev = b[1] - a[1], len2 = eu * eu + ev * ev;
                const double s = len2 > 0.0 ? std::clamp(((x - a[0]) * eu + (y - a[1]) * ev) / len2, 0.0, 1.0) : 0.0;
                best = std::min(best, std::hypot(x - a[0] - s * eu, y - a[1] - s * ev));
            }
        return best;
    };
    double depth = 0.0, gap = 1e300;
    size_t points = 0;
    for (const auto& [a, b] : {std::pair<const Loops*, const Loops*>{&f0, &f1}, {&f1, &f0}})
        for (const auto& l : *a)
            for (const auto& p : l)
            {
                ++points;
                const double d = nearest(*b, p[0], p[1]);
                if (inside(*b, p[0], p[1]))
                    depth = std::max(depth, d);
                else
                    gap = std::min(gap, d);
            }
    double size = 1e300;
    for (const Loops* l : {&f0, &f1})
    {
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (const auto& loop : *l)
            for (const auto& p : loop)
                x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
        size = std::min(size, std::max(x1 - x0, y1 - y0));
    }
    if (depth > 0.05 * size)
        r.Error(std::format("'{}' and '{}' overlap by {:.3f} mm at rest (at {:.3f} mm along their axis).", p0->name, p1->name, depth * 1e3, height * 1e3),
                "Draw them where they just touch, or apart: a touch pushes overlapping outlines apart at once.");
    r.s.measured.push_back(std::format("Touch {}: '{}' and '{}' at {:.3f} mm (heights overlap {:.3f} mm), {} outline points; at rest {}.", e.name, p0->name,
                                       p1->name, height * 1e3, (hi - lo) * 1e3, points,
                                       depth > 0.0 ? std::format("touching ({:.1f} um in)", depth * 1e6) : std::format("{:.3f} mm apart", gap * 1e3)));
    for (const auto& loop : o0)
        for (auto& out = t.outline0.emplace_back(); const Vec3& p : loop)
            out.push_back(Arr(p));
    for (const auto& loop : o1)
        for (auto& out = t.outline1.emplace_back(); const Vec3& p : loop)
            out.push_back(Arr(p));
    r.s.machine.mechanism.touches.push_back(t);
}

// ---- Magnetos -------------------------------------------------------------------------------------------------------

void ReadMagneto(Reader& r, const doc::ElementInfo& e)
{
    const int rotor = r.NeedJoint(e, "rotor");
    const doc::PartInfo* magnet = r.NeedPart(e, "magnet");
    if (rotor < 0 || !magnet)
        return;
    const sim::mb::Joint& J = r.s.machine.mechanism.joints[size_t(rotor)];
    const int side = r.Side(rotor, r.Body(magnet->body));
    if (!Rotational(J.type) || side == 0)
    {
        r.Error(std::format("The magnet '{}' of the magneto '{}' must turn with the rotor joint '{}' (a revolute joint of its body '{}').", magnet->name, e.name,
                            J.name, magnet->body),
                "Name the revolute joint the magnet's body turns on in 'rotor'.");
        return;
    }
    const int stator = side > 0 ? J.body1 : J.body0;
    const Polar polar(FromArr(J.point), FromArr(J.axis));
    sim::machine::Magneto m;
    m.name = e.name;
    m.rotor = rotor;
    m.sign = side;
    // The magnet: radial depth, angular width, position at rest.
    double magnetAngle = 0, lo = 0, hi = 0, rIn = 1e9, rOut = 0, zLo = 1e9, zHi = -1e9;
    std::vector<double> angles;
    for (const Vec3& p : r.Points(*magnet))
    {
        const double rad = Norm(Radial(p, polar.c, polar.a));
        rIn = std::min(rIn, rad), rOut = std::max(rOut, rad);
        zLo = std::min(zLo, polar.Along(p)), zHi = std::max(zHi, polar.Along(p));
        angles.push_back(polar.Angle(p));
    }
    AngularRange(angles, magnetAngle, lo, hi);
    const double segment = hi - lo;
    m.geometry.flywheelRadius = rOut;
    m.geometry.assemblyDepth = rOut - rIn;
    m.geometry.segmentAngleDeg = segment;
    const std::optional<doc::Material> magnetMaterial = r.d.FindMaterial(magnet->material);
    m.geometry.magnetRemanence = magnetMaterial ? magnetMaterial->remanence : 0.0;
    if (m.geometry.magnetRemanence <= 0.0)
        r.Error(std::format("The material '{}' of the magnet '{}' has no remanence: it is not a permanent magnet.", magnet->material, magnet->name),
                std::format("Give it one: aiad_material_define '{}' with remanence (ferrite 0.4 T, NdFeB 1.2 T).", magnet->material));
    // Pole shoes on both sides of the magnet (the field solver's rotor).
    const Json shoes = e.definition.value("shoes", Json::array());
    if (shoes.size() != 2)
        r.Error(std::format("The magneto '{}' has {} pole shoe(s): the field solver models a magnet between two steel pole shoes.", e.name, shoes.size()),
                "Draw a shoe of the magnet's width on each side of it and list both in 'shoes'.");
    for (const Json& name : shoes)
        if (const doc::PartInfo* shoe = r.Part(name))
        {
            std::vector<double> a;
            for (const Vec3& p : r.Points(*shoe))
                a.push_back(polar.Angle(p));
            double centre = 0, slo = 0, shi = 0;
            AngularRange(a, centre, slo, shi);
            const double offset = std::abs(Wrap180(centre - magnetAngle));
            if (std::abs(offset - segment) > 1.5 || std::abs((shi - slo) - segment) > 1.5)
                r.Error(std::format("The pole shoe '{}' spans {:.1f} deg, {:.1f} deg from the magnet's centre; the field solver needs it {:.1f} deg wide next to the "
                                    "magnet.",
                                    shoe->name, shi - slo, offset, segment),
                        "Draw the shoes as wide as the magnet, one on each side, touching it.");
            if (r.Side(rotor, r.Body(shoe->body)) != side)
                r.Error(std::format("The pole shoe '{}' does not turn with the magnet.", shoe->name), "Put it in the magnet's body.");
        }
        else
            r.Error(std::format("The magneto '{}' names the shoe '{}', which does not exist.", e.name, name.dump()), "Name existing parts in 'shoes'.");
    // The stator core: the two legs closest to the rotor and the yoke joining them.
    struct CorePart
    {
        const doc::PartInfo* part;
        double angle, inner;
    };
    std::vector<CorePart> core;
    for (const Json& name : e.definition.value("core", Json::array()))
        if (const doc::PartInfo* p = r.Part(name))
        {
            if (r.Body(p->body) != stator && !(stator < 0 && !r.Moving(r.Body(p->body))))
                r.Error(std::format("The core part '{}' is not on the magneto's stator ({}).", p->name, r.BodyName(stator)), "Put the core in the body the rotor turns in.");
            std::vector<double> a;
            double inner = 1e9;
            for (const Vec3& q : r.Points(*p))
                a.push_back(polar.Angle(q)), inner = std::min(inner, Norm(Radial(q, polar.c, polar.a)));
            double centre = 0, clo = 0, chi = 0;
            AngularRange(a, centre, clo, chi);
            core.push_back({p, centre, inner});
        }
    if (core.size() < 3)
    {
        r.Error(std::format("The magneto '{}' core has {} part(s): the field solver models a U core (two legs and a yoke).", e.name, core.size()),
                "List the two legs and the yoke in 'core'.");
        return;
    }
    std::sort(core.begin(), core.end(), [](const CorePart& a, const CorePart& b) { return a.inner < b.inner; });
    double coreAngle = 0, clo = 0, chi = 0;
    AngularRange({core[0].angle, core[1].angle}, coreAngle, clo, chi);
    const double legHalf = (chi - clo) / 2.0;
    if (std::abs(legHalf - segment) > 1.5)
        r.Error(std::format("The core legs are {:.1f} deg either side of the core axis; the field solver models them at the magnet's width ({:.1f} deg).", legHalf,
                            segment),
                "Turn the legs to the magnet's width either side of the core axis.");
    double legIn = 1e9, legLo = 1e9, legHi = -1e9, coreLo = 1e9, coreHi = -1e9;
    for (int k = 0; k < 2; ++k)
    {
        const Vec3 u = polar.Direction(core[size_t(k)].angle), t = Cross(polar.a, u);
        double wlo = 1e9, whi = -1e9;
        for (const Vec3& p : r.Points(*core[size_t(k)].part))
        {
            const Vec3 rad = Radial(p, polar.c, polar.a);
            legIn = std::min(legIn, Dot(rad, u));
            wlo = std::min(wlo, Dot(rad, t)), whi = std::max(whi, Dot(rad, t));
            coreLo = std::min(coreLo, polar.Along(p)), coreHi = std::max(coreHi, polar.Along(p));
        }
        legLo = std::min(legLo, whi - wlo), legHi = std::max(legHi, whi - wlo);
    }
    const Vec3 axisDir = polar.Direction(coreAngle);
    double yokeIn = 1e9, yokeOut = -1e9;
    for (size_t k = 2; k < core.size(); ++k)
        for (const Vec3& p : r.Points(*core[k].part))
        {
            const double along = Dot(Radial(p, polar.c, polar.a), axisDir);
            yokeIn = std::min(yokeIn, along), yokeOut = std::max(yokeOut, along);
        }
    m.geometry.airGap = legIn - rOut;
    m.geometry.legWidth = legHi;
    m.geometry.legLength = yokeIn - legIn;
    m.geometry.yokeThickness = yokeOut - yokeIn;
    m.geometry.stackDepth = coreHi - coreLo;
    if (const std::optional<doc::Material> steel = r.d.FindMaterial(core[0].part->material); steel && steel->relativePermeability > 0.0)
        m.geometry.coreRelativePermeability = steel->relativePermeability;
    if (m.geometry.airGap <= 0.0)
        r.Error(std::format("The magnet of '{}' reaches {:.2f} mm into its core legs: it would hit them.", e.name, -m.geometry.airGap * 1e3),
                "Leave an air gap (about 0.3 to 0.5 mm) between the rotor and the legs.");
    if (zHi < coreLo || zLo > coreHi)
        r.Error(std::format("The magnet of '{}' passes beside its core (along the axis: magnet {:.1f} to {:.1f} mm, core {:.1f} to {:.1f} mm).", e.name, zLo * 1e3,
                            zHi * 1e3, coreLo * 1e3, coreHi * 1e3),
                "Line the magnet up with the core along the rotor axis.");
    // Measured on a tessellation: rounded to what matters (1 um, 1e-4 deg), so that the same drawing always
    // gives the same field table (it is cached by its exact inputs).
    auto round = [](double v, double step) { return std::round(v / step) * step; };
    for (double* v : {&m.geometry.flywheelRadius, &m.geometry.assemblyDepth, &m.geometry.airGap, &m.geometry.legWidth, &m.geometry.legLength,
                      &m.geometry.yokeThickness, &m.geometry.stackDepth})
        *v = round(*v, 1e-6);
    m.geometry.segmentAngleDeg = round(m.geometry.segmentAngleDeg, 1e-4);
    m.atRestDeg = Wrap180(magnetAngle - coreAngle);
    m.circuit.pointsOpenDeg = r.Number(e, "points_open", m.circuit.pointsOpenDeg);
    m.circuit.pointsCloseDeg = r.Number(e, "points_close", m.circuit.pointsCloseDeg);
    m.circuit.primaryTurns = int(std::lround(r.Number(e, "primary_turns", m.circuit.primaryTurns)));
    m.circuit.secondaryTurns = int(std::lround(r.Number(e, "secondary_turns", m.circuit.secondaryTurns)));
    const double sparkAt = std::fmod((m.circuit.pointsOpenDeg - m.atRestDeg) * m.sign + 720.0, 360.0);
    r.s.measured.push_back(std::format("Magneto {}: magnet {:.1f} mm radius, {:.1f} deg wide, {:.2f} T; air gap {:.2f} mm, legs {:.1f} mm wide, stack {:.1f} mm. "
                                       "At rest the magnet is {:+.1f} deg from the core; the points open (spark) when '{}' has turned {:.1f} deg.",
                                       e.name, rOut * 1e3, segment, m.geometry.magnetRemanence, m.geometry.airGap * 1e3, m.geometry.legWidth * 1e3,
                                       m.geometry.stackDepth * 1e3, m.atRestDeg, J.name, sparkAt));
    r.s.machine.magnetos.push_back(m);
}

// ---- Chambers -----------------------------------------------------------------------------------------------------------

// A dynamometer: a brake on a shaft that measures what the machine gives there. Its controller's gains come from the
// inertia of the shaft's body about the axis (parallel axes from its centre of mass).
void ReadDynamometer(Reader& r, const doc::ElementInfo& e)
{
    const int j = r.NeedJoint(e, "joint");
    if (j < 0)
        return;
    const sim::mb::Joint& joint = r.s.machine.mechanism.joints[size_t(j)];
    if (joint.type != sim::mb::JointType::Revolute && joint.type != sim::mb::JointType::Cylindrical)
    {
        r.Error(std::format("The dynamometer '{}' is on '{}', which does not turn (a dynamometer brakes a shaft).", e.name, joint.name),
                "Put it on the revolute joint of the output shaft (a crankshaft's main bearing, a turbine's rotor).");
        return;
    }
    sim::machine::Dynamometer d;
    d.name = e.name;
    d.joint = j;
    const std::string mode = e.definition.value("mode", "speed");
    d.mode = mode == "sweep" ? sim::machine::Dynamometer::Mode::Sweep : mode == "torque" ? sim::machine::Dynamometer::Mode::Torque : sim::machine::Dynamometer::Mode::Speed;
    constexpr double rpm = 2.0 * std::numbers::pi / 60.0;
    d.speed = r.Number(e, "speed", 0.0) * rpm;
    d.from = r.Number(e, "from", 0.0) * rpm;
    d.to = r.Number(e, "to", 0.0) * rpm;
    d.seconds = r.Number(e, "seconds", 0.0);
    d.torque = r.Number(e, "torque", 0.0);
    d.maxTorque = r.Number(e, "max_torque", 1.0e4);
    d.start = r.Number(e, "start", 0.0);
    const char* missing = d.mode == sim::machine::Dynamometer::Mode::Speed && d.speed <= 0.0      ? "a held speed > 0 ('speed', rpm)"
                          : d.mode == sim::machine::Dynamometer::Mode::Sweep && (d.to <= 0.0 || d.seconds <= 0.0) ? "'from', 'to' (rpm) and 'seconds' > 0"
                          : d.mode == sim::machine::Dynamometer::Mode::Torque && d.torque <= 0.0 ? "a braking torque > 0 ('torque', N m)"
                          : d.maxTorque <= 0.0                                                    ? "'max_torque' > 0"
                                                                                                  : nullptr;
    if (missing)
    {
        r.Error(std::format("The dynamometer '{}' ({}) needs {}.", e.name, mode, missing), "Set it with aiad_element_define.");
        return;
    }
    // Inertia of the braked body about the shaft (the ground side when body0 is fixed).
    const int b = joint.body0 >= 0 && r.Moving(joint.body0) ? joint.body0 : joint.body1;
    if (b < 0 || !r.Moving(b))
    {
        r.Error(std::format("The dynamometer '{}' is on '{}', and no moving body turns there.", e.name, joint.name), "Put it on a joint that turns a body.");
        return;
    }
    const sim::mb::Body& body = r.s.machine.mechanism.bodies[size_t(b)];
    sim::mb::Vec3 a = joint.axis;
    const double length = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    for (double& c : a)
        c /= std::max(length, 1e-12);
    const auto& I = body.inertia;   // Ixx, Iyy, Izz, Iyz, Izx, Ixy
    const double axial = a[0] * a[0] * I[0] + a[1] * a[1] * I[1] + a[2] * a[2] * I[2] + 2.0 * (a[1] * a[2] * I[3] + a[2] * a[0] * I[4] + a[0] * a[1] * I[5]);
    const sim::mb::Vec3 rel{body.centroid[0] - joint.point[0], body.centroid[1] - joint.point[1], body.centroid[2] - joint.point[2]};
    const double along = rel[0] * a[0] + rel[1] * a[1] + rel[2] * a[2];
    const double off2 = rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2] - along * along;
    d.inertia = std::max(axial + body.mass * std::max(off2, 0.0), 1e-12);
    r.s.measured.push_back(std::format("Dynamometer {}: brakes '{}' ({}), shaft inertia {:.4g} kg m^2, absorbs up to {:.0f} N m, from {:.2f} s{}.", e.name,
                                       joint.name, d.mode == sim::machine::Dynamometer::Mode::Speed   ? std::format("holds {:.0f} rpm", d.speed / rpm)
                                                   : d.mode == sim::machine::Dynamometer::Mode::Sweep ? std::format("sweeps {:.0f} -> {:.0f} rpm in {:.1f} s", d.from / rpm, d.to / rpm, d.seconds)
                                                                                                      : std::format("{:.2f} N m", d.torque),
                                       d.inertia, d.maxTorque, d.start, ""));
    r.s.machine.dynamometers.push_back(d);
}

void ReadChamber(Reader& r, const doc::ElementInfo& e, const std::map<std::string, int>& magnetos)
{
    const int piston = r.NeedJoint(e, "piston");
    const doc::PartInfo* cylinder = r.NeedPart(e, "cylinder");
    const doc::PartInfo* head = r.NeedPart(e, "head");
    if (piston < 0 || !cylinder || !head)
        return;
    const sim::mb::Joint& J = r.s.machine.mechanism.joints[size_t(piston)];
    if (J.type != JointType::Prismatic)
    {
        r.Error(std::format("The piston joint '{}' of the chamber '{}' is not prismatic: a piston slides in its cylinder.", J.name, e.name),
                "Name the prismatic joint of the piston in 'piston'.");
        return;
    }
    const Vec3 a = Unit(FromArr(J.axis)), c = FromArr(J.point);
    // The piston's body: the side of the joint that is not the cylinder's.
    const int cylinderBody = r.Body(cylinder->body);
    const int pistonBody = J.body0 == cylinderBody || (cylinderBody >= 0 && !r.Moving(cylinderBody) && !r.Moving(J.body0)) ? J.body1 : J.body0;
    if (!r.Moving(pistonBody))
    {
        r.Error(std::format("The joint '{}' of the chamber '{}' does not slide a moving piston.", J.name, e.name), "Join the piston's body to the cylinder's.");
        return;
    }
    // Bore: the inside of the drawn cylinder about the piston's axis.
    double bore = 1e9;
    for (const Vec3& p : r.Points(*cylinder))
        bore = std::min(bore, 2.0 * Norm(Radial(p, c, a)));
    // The head on one side of the piston: the chamber is between the crown and the head's face.
    Vec3 headCentre;
    for (const Vec3& p : r.Points(*head))
        headCentre = Add(headCentre, Mul(p, 1.0 / double(r.Points(*head).size())));
    const Vec3 u = Mul(a, Dot(Sub(headCentre, c), a) >= 0.0 ? 1.0 : -1.0);
    double face = 1e9, crown = -1e9, pistonRadius = 0.0;
    for (const Vec3& p : r.Points(*head))
        face = std::min(face, Dot(p, u));
    std::vector<const doc::PartInfo*> pistonParts;
    for (const doc::PartInfo& p : r.parts)
        if (r.Body(p.body) == pistonBody)
        {
            pistonParts.push_back(&p);
            for (const Vec3& q : r.Points(p))
            {
                crown = std::max(crown, Dot(q, u));
                pistonRadius = std::max(pistonRadius, Norm(Radial(q, c, a)));
            }
        }
    const double gap = face - crown;
    if (bore > 1e8 || bore <= 0.0)
    {
        r.Error(std::format("The cylinder '{}' of the chamber '{}' has no bore around the piston's axis.", cylinder->name, e.name),
                "Name the part whose inside is the bore (the liner) in 'cylinder', and put the piston joint on its axis.");
        return;
    }
    if (pistonRadius > bore / 2.0 + 1e-5)
        r.Error(std::format("The piston of '{}' is {:.2f} mm across, larger than the {:.2f} mm bore of '{}'.", e.name, 2 * pistonRadius * 1e3, bore * 1e3, cylinder->name),
                "Make the piston slightly smaller than the bore.");
    if (gap <= 0.0)
    {
        r.Error(std::format("The piston crown of '{}' is {:.2f} mm past the face of the head '{}' at rest.", e.name, -gap * 1e3, head->name),
                "Draw the piston below the head (at top dead centre the crown stays a few mm from the head).");
        return;
    }
    // Chamber volume at rest, measured: the bore cylinder above the crown's highest point, plus the slab
    // under it not filled by the piston (crown shape, top land).
    const double area = kPi * bore * bore / 4.0, slab = 0.003;
    double filled = 0.0;
    const Vec3 o = r.d.BodyOrigin(r.s.bodies[size_t(pistonBody)]);
    for (const doc::PartInfo* p : pistonParts)
        filled += r.d.VolumeBeyond(p->path, Sub(Mul(u, crown - slab), o), u);   // the crown above the cut (cached)
    sim::machine::Chamber ch;
    ch.name = e.name;
    ch.piston = piston;
    ch.sign = (pistonBody == J.body0 ? 1.0 : -1.0) * Dot(a, u);
    ch.gapAtRest = gap;
    ch.cylinder.geometry.bore = bore;
    ch.cylinder.geometry.clearance = std::max(0.0, area * slab - filled);
    ch.cylinder.intake.pressure = r.Number(e, "intake_pressure", ch.cylinder.intake.pressure);
    ch.cylinder.intake.temperature = r.Number(e, "intake_temperature", ch.cylinder.intake.temperature);
    ch.cylinder.exhaust.pressure = r.Number(e, "exhaust_pressure", ch.cylinder.exhaust.pressure);
    ch.cylinder.exhaust.temperature = r.Number(e, "exhaust_temperature", ch.cylinder.exhaust.temperature);
    ch.cylinder.combustion.airFuelRatio = r.Number(e, "air_fuel_ratio", ch.cylinder.combustion.airFuelRatio);
    ch.cylinder.combustion.durationDeg = r.Number(e, "burn_duration", ch.cylinder.combustion.durationDeg);
    if (e.definition.contains("crank"))
        ch.crank = r.NeedJoint(e, "crank");
    // Valves: they open into the cylinder (away from the head), seat on their stop, something opens them
    // and something closes them.
    double diameters[2] = {0.0, 0.0};
    for (int kind = 0; kind < 2; ++kind)
        for (const Json& v : e.definition.value(kind == 0 ? "intake" : "exhaust", Json::array()))
        {
            const int joint = r.Joint(v.value("joint", Json()));
            const doc::PartInfo* valve = r.Part(v.value("part", Json()));
            if (joint < 0 || !valve)
            {
                r.Error(std::format("The chamber '{}' names a valve that does not exist: {}.", e.name, v.dump()), "Name the valve's joint and part.");
                continue;
            }
            const sim::mb::Joint& VJ = r.s.machine.mechanism.joints[size_t(joint)];
            const int valveBody = r.Body(valve->body);
            const int side = r.Side(joint, valveBody);
            if (VJ.type != JointType::Prismatic || side == 0)
            {
                r.Error(std::format("The valve '{}' must slide on its joint '{}' (prismatic, in the valve's body '{}').", valve->name, VJ.name, valve->body),
                        "Join the valve's body to the head with a prismatic joint along its stem.");
                continue;
            }
            const Vec3 va = Unit(FromArr(VJ.axis)), vc = FromArr(VJ.point);
            double radius = 0.0;
            for (const Vec3& p : r.Points(*valve))
                radius = std::max(radius, Norm(Radial(p, vc, va)));
            diameters[kind] += 2.0 * radius;
            sim::machine::ChamberValve cv;
            cv.joint = joint;
            cv.sign = -side * Dot(va, u);   // opening = toward the piston
            (kind == 0 ? ch.intake : ch.exhaust).push_back(cv);
            if (Norm(Radial(vc, c, a)) + radius > bore / 2.0 + 0.0005)
                r.Error(std::format("The valve '{}' ({:.1f} mm, {:.1f} mm off the cylinder axis) does not open into the {:.1f} mm bore.", valve->name, 2 * radius * 1e3,
                                    Norm(Radial(vc, c, a)) * 1e3, bore * 1e3),
                        "Move or shrink the valve to fit inside the bore.");
            const bool closesUp = cv.sign < 0;   // closing = increasing coordinate
            if (closesUp ? !VJ.upper : !VJ.lower)
                r.Error(std::format("The valve '{}' has no seat: nothing stops it where it closes.", valve->name),
                        std::format("Give its joint '{}' a stop at the drawn (closed) position: limits {}.", VJ.name, closesUp ? "[null, 0]" : "[0, null]"));
            bool opened = false, closed = false;
            for (const sim::mb::Contact& ct : r.s.machine.mechanism.contacts)
                opened = opened || ct.follower == valveBody;
            for (const sim::mb::Spring& sp : r.s.machine.mechanism.springs)
                closed = closed || sp.body0 == valveBody || sp.body1 == valveBody;
            for (const sim::mb::Coupling& cp : r.s.machine.mechanism.couplings)
                opened = opened || cp.joint0 == joint || cp.joint1 == joint;
            if (!opened && VJ.motor.mode == sim::mb::Motor::Mode::None)
                r.Error(std::format("Nothing opens the valve '{}': no cam lobe or rocker acts on its body '{}'.", valve->name, valve->body),
                        "Add a contact element: the lobe or rocker pad (part) on the valve's follower face (body, point, normal, radius).");
            if (!closed)
                r.Error(std::format("Nothing closes the valve '{}': no spring holds it on its seat.", valve->name),
                        "Draw its valve spring and add a spring element between the head and the valve's body.");
        }
    ch.cylinder.intakeValve.diameter = diameters[0];
    ch.cylinder.exhaustValve.diameter = diameters[1];
    if (ch.intake.empty())
        r.Error(std::format("The chamber '{}' has no intake valve: no fresh charge comes in.", e.name), "List its intake valves (joint and part) in 'intake'.");
    if (e.definition.contains("ignition"))
    {
        const auto it = magnetos.find(e.definition.value("ignition", ""));
        if (it == magnetos.end())
            r.Error(std::format("The chamber '{}' is fired by '{}', which is not a magneto element.", e.name, e.definition["ignition"].dump()),
                    "Name the magneto element in 'ignition'.");
        else
            ch.ignition = it->second;
    }
    else
        r.Note(std::format("Nothing ignites the chamber '{}': it only compresses and pumps.", e.name), "Name its magneto in 'ignition'.");
    r.s.measured.push_back(std::format("Chamber {}: bore {:.2f} mm (inside of '{}'), piston crown {:.2f} mm from '{}' at rest, chamber {:.2f} cm^3 at rest; valve "
                                       "diameters {:.1f} mm intake, {:.1f} mm exhaust (measured on the valves).",
                                       e.name, bore * 1e3, cylinder->name, gap * 1e3, head->name, (area * gap + ch.cylinder.geometry.clearance) * 1e6,
                                       diameters[0] * 1e3, diameters[1] * 1e3));
    r.s.machine.chambers.push_back(ch);
}

}  // namespace

bool MechanismStudy::Runnable() const
{
    return Errors() == 0;
}

int MechanismStudy::Errors() const
{
    return int(std::count_if(findings.begin(), findings.end(), [](const StudyFinding& f) { return f.error; }));
}

std::string MechanismStudy::Report() const
{
    std::string text;
    for (const StudyFinding& f : findings)
        text += std::format("- {}{}\n  Fix: {}\n", f.error ? "" : "(note) ", f.problem, f.fix);
    return text;
}

bool DeriveMechanismStudy(const doc::Document& d, MechanismStudy& study)
{
    study = MechanismStudy();
    const std::vector<doc::JointInfo> joints = d.Joints();
    if (joints.empty())
        return false;
    Reader r{d, study, d.Parts()};
    const std::vector<doc::ElementInfo> elements = d.Elements();
    sim::mb::Model& model = study.machine.mechanism;

    // Spring bodies: bodies that hold only drawn springs (stretched in the view, not rigid).
    std::set<std::string> springParts, springBodies;
    for (const doc::ElementInfo& e : elements)
        if (e.type == "spring")
            springParts.insert(e.definition.value("part", ""));
    for (const doc::PartInfo& p : r.parts)
        if (springParts.contains(p.name))
            springBodies.insert(p.body);
    for (const doc::PartInfo& p : r.parts)
        if (!springParts.contains(p.name))
            springBodies.erase(p.body);

    // Bodies: mass, centre of mass and inertia tensor (model axes) from their parts x materials; the
    // extent of their parts (to check where the joints are).
    std::map<std::string, Box> extents;
    for (const std::string& name : d.Bodies())
    {
        if (springBodies.contains(name))
            continue;
        const kernel::Vec3 origin = d.BodyOrigin(name);
        sim::mb::Body body;
        body.name = name;
        body.fixed = d.BodyFixed(name);
        std::vector<kernel::MassProperties> list;
        for (const doc::PartInfo& p : r.parts)
        {
            if (p.body != name)
                continue;
            // A fixed body does not move: its mass is no input (and the exact mass of a large imported part costs
            // seconds); its parts only need a valid solid.
            kernel::MassProperties mp;
            std::string e;
            if (body.fixed)
            {
                const kernel::Solid* solid = d.Shape(p.path);
                if (!solid || !solid->Valid())
                    r.Error(std::format("'{}' has no valid solid.", p.name), "Fix its features (Properties panel, or aiad_part_update) until it regenerates.");
            }
            else if (!d.Mass(p.path, mp, e))
                r.Error(std::format("'{}' has no valid solid, so its mass is unknown: {}.", p.name, e),
                        "Fix its features (Properties panel, or aiad_part_update) until it regenerates.");
            else if (mp.mass > 0.0)
                list.push_back(mp);
            if (kernel::Vec3 lo, hi; d.Bounds(p.path, lo, hi))
                extents[name].Add(Add(lo, origin), Add(hi, origin));
        }
        double c[3] = {0, 0, 0};
        for (const kernel::MassProperties& mp : list)
        {
            body.mass += mp.mass;
            c[0] += mp.mass * (origin.x + mp.centroid.x);
            c[1] += mp.mass * (origin.y + mp.centroid.y);
            c[2] += mp.mass * (origin.z + mp.centroid.z);
        }
        if (body.mass > 0.0)
            body.centroid = {c[0] / body.mass, c[1] / body.mass, c[2] / body.mass};
        else
            body.centroid = {origin.x, origin.y, origin.z};
        // Parallel axes, tensor entries (off-diagonal entries are the negated products of inertia).
        for (const kernel::MassProperties& mp : list)
        {
            const double dx = origin.x + mp.centroid.x - body.centroid[0], dy = origin.y + mp.centroid.y - body.centroid[1],
                         dz = origin.z + mp.centroid.z - body.centroid[2];
            body.inertia[0] += mp.inertia[0] + mp.mass * (dy * dy + dz * dz);
            body.inertia[1] += mp.inertia[1] + mp.mass * (dz * dz + dx * dx);
            body.inertia[2] += mp.inertia[2] + mp.mass * (dx * dx + dy * dy);
            body.inertia[3] += mp.inertia[3] - mp.mass * dy * dz;
            body.inertia[4] += mp.inertia[4] - mp.mass * dz * dx;
            body.inertia[5] += mp.inertia[5] - mp.mass * dx * dy;
        }
        if (!body.fixed && body.mass <= 0.0)
            r.Error(std::format("The body '{}' has no parts, so it has no mass: it cannot move.", name),
                    std::format("Draw its parts in it (aiad_part_create with body '{}'), or make it part of the ground (aiad_body_define fixed: true).", name));
        study.origins[name] = origin;
        study.bodies.push_back(name);
        model.bodies.push_back(body);
    }
    for (const std::string& name : springBodies)
        study.origins[name] = d.BodyOrigin(name);

    // Joints, with their point checked against the parts they connect, their motor and their stops.
    for (const doc::JointInfo& j : joints)
    {
        sim::mb::Joint joint;
        joint.name = j.name;
        joint.type = TypeOf(j.type);
        joint.body0 = r.Body(j.body0);
        joint.body1 = j.body1.empty() ? -1 : r.Body(j.body1);
        for (const std::string& b : {j.body0, j.body1})
            if (springBodies.contains(b))
                r.Error(std::format("The joint '{}' is on '{}', a body that only holds a spring (it is stretched, not rigid).", j.name, b),
                        "Join the bodies at the spring's ends instead; the spring element connects them.");
        kernel::Vec3 point, axis;
        std::string e;
        if (!d.EvaluateVector(j.point, point, e) || !d.EvaluateVector(j.axis, axis, e))
        {
            r.Error(std::format("The joint '{}' cannot be placed: {}.", j.name, e), "Fix its point and axis (aiad_joint_define).");
            continue;
        }
        joint.point = Arr(point);
        joint.axis = Arr(axis);
        if (j.motor.is_object())
        {
            joint.motor.mode = j.motor.value("mode", "") == "torque" ? sim::mb::Motor::Mode::Torque : sim::mb::Motor::Mode::Speed;
            for (const auto& [key, target] : {std::pair<const char*, double*>{"value", &joint.motor.value}, {"free_speed", &joint.motor.freeSpeed},
                                              {"duration", &joint.motor.duration}})
                if (j.motor.contains(key) && !d.EvaluateNumber(j.motor[key], *target, e))
                    r.Error(std::format("The motor of '{}': {} cannot be evaluated: {}.", j.name, key, e), "Fix the motor (aiad_joint_define).");
        }
        if (j.limits.is_array())
            for (size_t i = 0; i < 2 && i < j.limits.size(); ++i)
                if (double v = 0; !j.limits[i].is_null() && d.EvaluateNumber(j.limits[i], v, e))
                    (i == 0 ? joint.lower : joint.upper) = v;
        // Where it starts: a value, or the local time when the simulation starts (read then by the solver).
        if (j.start.is_object())
        {
            if (!d.EvaluateNumber(j.start.value("local_time", Json(0)), joint.clockPeriod, e) ||
                (j.start.contains("offset") && !d.EvaluateNumber(j.start["offset"], joint.start, e)))
                r.Error(std::format("The start of '{}' cannot be evaluated: {}.", j.name, e), "Fix its start (aiad_joint_define).");
        }
        else if (!j.start.is_null() && !d.EvaluateNumber(j.start, joint.start, e))
            r.Error(std::format("The start of '{}' cannot be evaluated: {}.", j.name, e), "Fix its start (aiad_joint_define).");
        if (joint.clockPeriod != 0.0)
            r.s.measured.push_back(std::format("Joint {}: starts at the local time, one turn per {:g} s{} (offset {:.4f} rad at 00:00:00).", j.name,
                                               std::abs(joint.clockPeriod), joint.clockPeriod < 0.0 ? ", the other way" : "", joint.start));
        else if (joint.start != 0.0)
            r.s.measured.push_back(std::format("Joint {}: starts at {:g} {} from the drawn position.", j.name, joint.start,
                                               joint.type == sim::mb::JointType::Prismatic ? "m" : "rad"));
        for (const std::string& body : {j.body0, j.body1})
        {
            if (body.empty())
                continue;
            const auto it = extents.find(body);
            if (it != extents.end() && !it->second.Empty() && !it->second.Contains(point, 0.001))
                r.Error(std::format("The joint '{}' is at {}, outside the parts of '{}' (they span {} to {}): the two bodies are not joined "
                                    "where they are drawn.",
                                    j.name, Mm(point), body, Mm(it->second.lo), Mm(it->second.hi)),
                        std::format("Put the joint where '{}' and '{}' meet (for a pin in a hole: on the pin's axis), with aiad_joint_define.", j.body0,
                                    j.body1.empty() ? "the ground" : j.body1));
        }
        model.joints.push_back(joint);
    }

    // Elements: loads, couplings, contacts and springs (the mechanism), then magnetos and chambers.
    std::map<std::string, int> magnetos;
    for (const char* kind : {"load", "gear", "belt", "rack", "contact", "spring", "torsion", "touch", "dynamometer", "magneto", "chamber"})
        for (const doc::ElementInfo& e : elements)
        {
            if (e.type != kind)
                continue;
            if (e.type == "load")
            {
                if (const int j = r.NeedJoint(e, "joint"); j >= 0)
                {
                    sim::mb::JointLoad& load = model.joints[size_t(j)].load;
                    load.constant += r.Number(e, "constant", 0.0);
                    load.linear += r.Number(e, "linear", 0.0);
                    load.quadratic += r.Number(e, "quadratic", 0.0);
                }
            }
            else if (e.type == "gear" || e.type == "belt" || e.type == "rack")
                ReadCoupling(r, e);
            else if (e.type == "contact")
                ReadContact(r, e);
            else if (e.type == "spring")
                ReadSpring(r, e, springBodies);
            else if (e.type == "torsion")
                ReadTorsion(r, e);
            else if (e.type == "touch")
                ReadTouch(r, e);
            else if (e.type == "dynamometer")
                ReadDynamometer(r, e);
            else if (e.type == "magneto")
            {
                const size_t before = study.machine.magnetos.size();
                ReadMagneto(r, e);
                if (study.machine.magnetos.size() > before)
                    magnetos[e.name] = int(before);
            }
            else
                ReadChamber(r, e, magnetos);
        }

    // Adjusters: a joint with a range on both sides that nothing drives is set by hand (a regulator, a screw, a lever):
    // it is held at its start while the machine runs, and the user (or the agent) moves it.
    {
        std::set<int> drivenJoints, drivenBodies;
        for (const sim::mb::Coupling& c : model.couplings)
            drivenJoints.insert(c.joint0), drivenJoints.insert(c.joint1);
        for (const sim::mb::TorsionSpring& t : model.torsions)
            drivenJoints.insert(t.joint);
        for (const sim::mb::Touch& t : model.touches)
            drivenBodies.insert(t.body0), drivenBodies.insert(t.body1);
        for (const sim::mb::Contact& c : model.contacts)
            drivenBodies.insert(c.body), drivenBodies.insert(c.follower);
        for (const sim::mb::Spring& s : model.springs)
            drivenBodies.insert(s.body0), drivenBodies.insert(s.body1);
        for (size_t j = 0; j < model.joints.size(); ++j)
        {
            sim::mb::Joint& joint = model.joints[j];
            const bool rotation = joint.type == sim::mb::JointType::Revolute || joint.type == sim::mb::JointType::Cylindrical;
            if ((!rotation && joint.type != sim::mb::JointType::Prismatic) || !joint.lower || !joint.upper || *joint.lower >= *joint.upper ||
                joint.motor.mode != sim::mb::Motor::Mode::None || joint.clockPeriod != 0.0 || drivenJoints.contains(int(j)))
                continue;
            const int moved = r.Moving(joint.body0) ? joint.body0 : joint.body1;
            if (moved < 0 || !r.Moving(moved) || drivenBodies.contains(joint.body0) || drivenBodies.contains(joint.body1))
                continue;
            if (joint.start < *joint.lower || joint.start > *joint.upper)
            {
                r.Error(std::format("The adjuster '{}' is set at {:.4g}, outside its range {:.4g} .. {:.4g}.", joint.name, joint.start, *joint.lower,
                                    *joint.upper),
                        "Set it inside its range (aiad_adjust), or widen its limits (aiad_joint_define).");
                continue;
            }
            study.adjusters.push_back({int(j), joint.name, study.bodies[size_t(moved)], rotation, *joint.lower, *joint.upper, joint.start});
            r.s.measured.push_back(std::format("Adjuster {}: {} is set by hand, {} {:.4g} {} (range {:.4g} .. {:.4g}); held there while it runs.",
                                               joint.name, r.BodyName(moved), rotation ? "turned" : "slid", joint.start, rotation ? "rad" : "m",
                                               *joint.lower, *joint.upper));
            joint.lower = joint.upper = joint.start;   // held where it is set
        }
    }

    // Ground and connection: every moving body must hang from the ground through joints.
    std::set<int> grounded;
    for (size_t i = 0; i < model.bodies.size(); ++i)
        if (model.bodies[i].fixed)
            grounded.insert(int(i));
    const bool toGround = std::any_of(model.joints.begin(), model.joints.end(), [](const sim::mb::Joint& j) { return j.body1 < 0; });
    if (grounded.empty() && !toGround)
    {
        // Suggest the heaviest body as the base.
        const auto heaviest = std::max_element(model.bodies.begin(), model.bodies.end(), [](const sim::mb::Body& a, const sim::mb::Body& b) { return a.mass < b.mass; });
        r.Error("Nothing holds the mechanism: no body is fixed and no joint goes to the ground, so the whole machine floats.",
                std::format("Make the base fixed (aiad_body_define body '{}' with fixed: true), or give a joint body1 'ground'.",
                            heaviest == model.bodies.end() ? std::string("Frame") : heaviest->name));
    }
    else
    {
        std::set<int> reached = grounded;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (const sim::mb::Joint& j : model.joints)
            {
                const bool a = j.body0 >= 0 && reached.contains(j.body0), b = j.body1 < 0 || reached.contains(j.body1);
                if (a != b)
                {
                    reached.insert(a ? j.body1 : j.body0);
                    grew = true;
                }
            }
            reached.erase(-1);
        }
        for (size_t i = 0; i < model.bodies.size(); ++i)
            if (!reached.contains(int(i)) && model.bodies[i].mass > 0.0)
                r.Error(std::format("'{}' is not connected to the ground through joints: it would fall or float away.", study.bodies[i]),
                        std::format("Mount it with a joint (aiad_joint_define) to the body it is attached to (revolute for a shaft or pin, "
                                    "prismatic for a slide, fixed if it does not move relative to it)."));
    }

    // Mobility, and what drives the machine.
    study.mobility = sim::mb::Mobility(model);
    const bool driven = !study.machine.chambers.empty() ||
                        std::any_of(model.joints.begin(), model.joints.end(), [](const sim::mb::Joint& j) { return j.motor.mode != sim::mb::Motor::Mode::None; }) ||
                        std::any_of(model.torsions.begin(), model.torsions.end(), [](const sim::mb::TorsionSpring& t) { return t.free != 0.0; });
    if (study.mobility < 0)
        r.Note(std::format("The joints over-constrain the mechanism ({} by the Gruebler-Kutzbach count): some constraints are redundant. It "
                           "still runs if the drawing is consistent; if it locks, a joint does not match the geometry.",
                           -study.mobility),
               "Replace a redundant joint by a looser one (e.g. a revolute by a spherical or cylindrical) where the parts allow it.");
    else if (study.mobility > 0 && !driven)
        r.Note(std::format("The mechanism has {} degree(s) of freedom and nothing drives it, so it stays at rest.", study.mobility),
               "Add a motor to the input joint (aiad_joint_define with motor {\"mode\": \"speed\", \"value\": rad/s}).");
    return true;
}

}  // namespace aiad::app
