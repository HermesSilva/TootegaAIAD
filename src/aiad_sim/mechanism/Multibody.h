// Generic multibody mechanism (Project Chrono) built from a description: rigid bodies with their mass
// properties, the joints between them (with motors, stops and resisting loads), couplings between joint
// coordinates (gears, belts, racks), springs and profile contacts (cams, rocker pads). No machine is known
// here: the description is read from the document (bodies, their parts x materials, joints, elements).
#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aiad::sim::mb
{

using Vec3 = std::array<double, 3>;
using Quat = std::array<double, 4>;   // w, x, y, z

struct Body
{
    std::string name;
    bool fixed = false;                // part of the ground
    double mass = 0.0;                 // [kg]
    Vec3 centroid{};                   // rest position of the centre of mass, model frame [m]
    std::array<double, 6> inertia{};   // about the centroid, model axes [kg m^2]: Ixx, Iyy, Izz, Iyz, Izx, Ixy
};

enum class JointType
{
    Revolute,      // rotation about the axis through the point
    Prismatic,     // sliding along the axis
    Cylindrical,   // both
    Spherical,     // rotation about the point
    Planar,        // motion in the plane through the point (axis = normal)
    Fixed
};

struct Motor
{
    enum class Mode
    {
        None,
        Speed,    // rad/s (rotation) or m/s (sliding): drives the joint coordinate
        Torque    // N m or N
    } mode = Mode::None;
    double value = 0.0;
    // Torque motors: freeSpeed > 0 gives the DC-motor characteristic T = value (1 - rate / freeSpeed),
    // never negative (a starter); duration > 0 switches the motor off at that time [s].
    double freeSpeed = 0.0, duration = 0.0;
};

// Resisting load on a joint (friction, brake, propeller): -(constant tanh(rate / 0.5) + linear rate +
// quadratic rate |rate|), torque about / force along the axis.
struct JointLoad
{
    double constant = 0.0, linear = 0.0, quadratic = 0.0;
};

struct Joint
{
    std::string name;
    JointType type = JointType::Revolute;
    int body0 = -1, body1 = -1;   // index into Model::bodies; -1 = the ground
    Vec3 point{};                 // model frame [m]
    Vec3 axis{0.0, 0.0, 1.0};
    Motor motor;
    std::optional<double> lower, upper;   // stops on the coordinate (rad or m): a seat, an end stop
    JointLoad load;
};

// rate(joint1) = ratio * rate(joint0): meshing gears (ratio -z0/z1), belts and chains (+z0/z1), rack and
// pinion (pitch radius, m/rad). Joints are revolute/cylindrical (rotation) or prismatic (sliding).
struct Coupling
{
    std::string name;
    int joint0 = -1, joint1 = -1;
    double ratio = 1.0;
};

// Linear spring-damper between a point of each body (model frame at rest): pushes them apart when shorter
// than its free length.
struct Spring
{
    std::string name;
    int body0 = -1, body1 = -1;
    Vec3 point0{}, point1{};
    double stiffness = 0.0, freeLength = 0.0;   // [N/m], [m]
    double damping = 0.0;                       // [N s/m]
};

// A drawn profile (a cam lobe, a rocker pad: points of its outline, model frame at rest) pushing a flat face
// of another body (the follower: a tappet, a valve tip). The face: its centre and its normal (pointing
// toward the profile) at rest, and its radius. Unilateral penalty contact; damping < 0 = automatic.
struct Contact
{
    std::string name;
    int body = -1;                 // the profile's body
    std::vector<Vec3> profile;
    int follower = -1;
    Vec3 point{}, normal{1.0, 0.0, 0.0};
    double radius = 0.0;
    double stiffness = 1.0e7, damping = -1.0;
};

struct Model
{
    std::vector<Body> bodies;
    std::vector<Joint> joints;
    std::vector<Coupling> couplings;
    std::vector<Spring> springs;
    std::vector<Contact> contacts;
    Vec3 gravity{0.0, 0.0, 0.0};
    double stopStiffness = 1.0e7;            // joint stops: sliding [N/m]
    double stopRotationalStiffness = 1.0e5;  // ... rotation [N m/rad]
};

// Rigid motion of a body from its rest placement: p = centroid + rotation * (p_rest - rest centroid).
struct BodyState
{
    Vec3 centroid{};
    Quat rotation{1.0, 0.0, 0.0, 0.0};
    Vec3 velocity{}, angularVelocity{};
};

// Degrees of freedom (Gruebler-Kutzbach): 6 per moving body (3 when the mechanism is planar) minus the
// constraints of the joints, speed motors and couplings fixing one more each. 0 = driven or locked;
// negative = over-constrained.
int Mobility(const Model& model);

class Multibody
{
public:
    explicit Multibody(const Model& model);
    ~Multibody();

    void Step(double dt);
    double Time() const;
    // A time step the force elements (contacts, stops, springs) stay stable with [s].
    double StableStep() const;
    BodyState State(int body) const;
    // Coordinate of a revolute/cylindrical joint (angle of body0 relative to body1 about the axis, rad)
    // or a prismatic one (displacement along the axis, m), and its rate (rad/s or m/s).
    double Coordinate(int joint) const;
    double Rate(int joint) const;
    // External loads for this step (cleared after it): torque about / force along a joint axis on body0
    // (and the reaction on body1), e.g. a domain element (gas pressure, fluid torque).
    void ApplyJointLoad(int joint, double value);
    // Last force of a contact (0 when apart) and of a spring (positive = compressed) [N], spring length [m].
    double ContactForce(int contact) const;
    double SpringForce(int spring) const;
    double SpringLength(int spring) const;
    // World position of a point of a body given at rest (model frame).
    Vec3 PointOf(int body, const Vec3& restPoint) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace aiad::sim::mb
