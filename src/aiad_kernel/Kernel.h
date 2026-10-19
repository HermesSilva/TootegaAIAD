// Geometry kernel facade (NVIDIA USD BRep / SMLib). Generic CAD operations only: sketches on planes
// (lines and arcs), extrusion, revolution, primitives, helical pipes, booleans, plane sections, mass
// properties, tessellation, BrepArray read/write. No object-specific geometry lives here or anywhere
// in code: shapes come from documents (features) through the Command Bus.
//
// The only file of the project that includes SMLib headers is Kernel.cpp (compiled as C++17); this
// header exposes no SMLib type.
#pragma once

#include <pxr/usd/usd/prim.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace aiad::kernel
{

struct Vec3
{
    double x = 0.0, y = 0.0, z = 0.0;
};

// Sketch plane: origin and two orthonormal in-plane axes; normal = xAxis x yAxis.
struct Plane
{
    Vec3 origin{0, 0, 0}, xAxis{1, 0, 0}, yAxis{0, 1, 0};
    Vec3 Normal() const;
    Vec3 ToWorld(double u, double v) const;
};

// Closed profile in plane coordinates. Each vertex starts a segment to the next one (the last
// closes on the first): a line, or an arc when bulge != 0 (bulge = tan(sweep / 4), positive = CCW).
struct ProfileVertex
{
    double u = 0.0, v = 0.0, bulge = 0.0;
};
using Profile = std::vector<ProfileVertex>;

class Solid
{
public:
    Solid();
    ~Solid();
    Solid(Solid&&) noexcept;
    Solid& operator=(Solid&&) noexcept;
    Solid(const Solid&) = delete;
    Solid& operator=(const Solid&) = delete;

    bool Valid() const;
    Solid Copy() const;

    struct Impl;
    Impl* impl() const { return m.get(); }

private:
    std::unique_ptr<Impl> m;
    friend struct Access;
};

// Every operation reports failure through `error` (empty = success) and returns an invalid Solid.
Solid Extrude(const Plane& plane, const Profile& profile, double distance, std::string& error);
// Revolves about the in-plane axis through (axisU, axisV) with direction (dirU, dirV).
Solid Revolve(const Plane& plane, const Profile& profile, double axisU, double axisV, double dirU, double dirV, double angleDeg,
              std::string& error);
// Smooth closed profile (a periodic-looking spline through the points, e.g. a cam lobe) extruded.
Solid ExtrudeSmooth(const Plane& plane, const std::vector<std::array<double, 2>>& points, double distance, std::string& error);
Solid Box(const Vec3& corner, const Vec3& size, std::string& error);
Solid Cylinder(const Vec3& base, const Vec3& axis, double radius, double height, std::string& error);
Solid Sphere(const Vec3& centre, double radius, std::string& error);
Solid Torus(const Vec3& centre, const Vec3& axis, double majorRadius, double minorRadius, std::string& error);
// Round wire wound on a helix (springs): axis from `base` along `axis`.
Solid HelicalPipe(const Vec3& base, const Vec3& axis, double coilRadius, double height, double turns, double wireRadius, std::string& error);
// Round pipe along a smooth path through the points (at least 4).
Solid Pipe(const std::vector<Vec3>& path, double radius, std::string& error);

enum class BooleanOp
{
    Union,
    Subtract,
    Intersect
};
// a = a op b. b is consumed.
bool Boolean(Solid& a, Solid&& b, BooleanOp op, std::string& error);

// Rigid placement: rotation about `axis` through `pivot` (degrees), then translation.
bool Rotate(Solid& solid, const Vec3& pivot, const Vec3& axis, double angleDeg, std::string& error);
bool Translate(Solid& solid, const Vec3& offset, std::string& error);

// The part on the side opposite to `normal` of the plane through `point` (section view), with its
// cut faces. Invalid (no error) when nothing remains on that side.
Solid KeepBelow(const Solid& solid, const Vec3& point, const Vec3& normal, std::string& error);

struct MassProperties
{
    double volume = 0.0;              // [m^3]
    double area = 0.0;                // [m^2]
    double mass = 0.0;                // [kg]
    Vec3 centroid;                    // [m]
    std::array<double, 6> inertia{};  // about the centroid, world axes: Ixx, Iyy, Izz, Iyz, Izx, Ixy (tensor entries)
};
bool ComputeMass(const Solid& solid, double density, MassProperties& out, std::string& error);
bool BoundingBox(const Solid& solid, Vec3& minimum, Vec3& maximum);
bool IsManifoldSolid(const Solid& solid);
int FaceCount(const Solid& solid);

struct Mesh
{
    std::vector<std::array<float, 3>> points, normals;  // per vertex
    std::vector<std::array<int, 3>> triangles;
};
// chordTolerance [m]; angleDeg: surface angle tolerance.
bool Tessellate(const Solid& solid, double chordTolerance, double angleDeg, Mesh& out, std::string& error);

// Section view of a tessellated part: the triangles on the side opposite to `normal` (clipped at the
// plane) and the cap that closes the cut (planar faces, holes included, normal = `normal`). Display
// only: the exact geometry is not changed.
void SectionMesh(const Mesh& mesh, const Vec3& point, const Vec3& normal, Mesh& kept, Mesh& cap);
double MeshArea(const Mesh& mesh);

// BrepArray prim <-> solids (the document stores the exact geometry, not meshes).
bool WriteToPrim(const std::vector<const Solid*>& solids, pxr::UsdPrim& brepArray, std::string& error);
std::vector<Solid> ReadFromPrim(const pxr::UsdPrim& brepArray, std::string& error);

// Must be called once before any operation (creates the SMLib context; registers the omniSolid
// schema plugin directory given in OMNISOLID_PLUGIN_PATH).
void Initialize();

}  // namespace aiad::kernel
