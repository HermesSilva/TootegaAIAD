// SMLib-backed implementation of the kernel facade. Compiled as C++17 (SMLib headers are not
// C++20-clean) with SM_STATIC_LINK, SM_GRAPHICS_CALLBACKS=1, UNICODE.
#include "Kernel.h"

#include <SmApiBrep.h>
#include <SmApiCurves.h>
#include <SmApiGeneral.h>
#include <SmApiPrimitives.h>
#include <SmApiQueries.h>
#include <SmApiUsd.h>
#include <SmBSplineCurve.h>
#include <SmBrep.h>
#include <SmContext.h>
#include <SmCurve.h>
#include <SmLine.h>
#include <SmPoly.h>
#include <SmVector3d.h>
#include <SmuConvert.h>
#include <SmuTessellate.h>

#include <cmath>
#include <algorithm>
#include <pxr/base/gf/vec3d.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace aiad::kernel
{

struct Solid::Impl
{
    SmBrep* brep = nullptr;
    ~Impl() { delete brep; }
};

struct Access
{
    static Solid Wrap(SmBrep* brep)
    {
        Solid s;
        s.m = std::make_unique<Solid::Impl>();
        s.m->brep = brep;
        return s;
    }
    static SmBrep* Get(const Solid& s) { return s.m ? s.m->brep : nullptr; }
    static SmBrep* Release(Solid& s)
    {
        SmBrep* b = s.m ? s.m->brep : nullptr;
        if (s.m)
            s.m->brep = nullptr;
        s.m.reset();
        return b;
    }
};

namespace
{

constexpr double kPi = 3.14159265358979323846;

SmVector3d V(const Vec3& v)
{
    return SmVector3d(v.x, v.y, v.z);
}

Vec3 FromSm(const SmVector3d& v)
{
    return {v[0], v[1], v[2]};
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double Dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

double Length(const Vec3& a)
{
    return std::sqrt(Dot(a, a));
}

Vec3 Unit(const Vec3& a)
{
    const double l = Length(a);
    return l > 0 ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{0, 0, 1};
}

bool Fail(std::string& error, const std::string& what, SmApiStatus status)
{
    error = what + " failed (SMLib status " + std::to_string(int(status)) + ")";
    return false;
}

// Places a solid built in the local XY frame (u, v, w) onto `plane`.
bool PlaceOnPlane(SmBrep* brep, const Plane& plane, std::string& error)
{
    const Vec3 x = Unit(plane.xAxis), y = Unit(plane.yAxis), n = Unit(Cross(x, y));
    // Column-major 4x4 (X axis, Y axis, normal, origin), as SetFrom4x4 reads it.
    double m[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, n.x, n.y, n.z, 0, plane.origin.x, plane.origin.y, plane.origin.z, 1};
    SmAxis2Placement placement;
    placement.SetFrom4x4(m);
    const SmApiStatus s = SmApiTransform(brep, placement);
    return s == SM_SUCCESS || Fail(error, "placing the sketch", s);
}

// Profile -> closed chain of curves in the local XY plane (caller deletes them).
bool ProfileCurves(const Profile& profile, SmTArray<SmCurve*>& curves, std::string& error)
{
    if (profile.size() < 2)
    {
        error = "a profile needs at least 2 vertices";
        return false;
    }
    for (size_t i = 0; i < profile.size(); ++i)
    {
        const ProfileVertex& a = profile[i];
        const ProfileVertex& b = profile[(i + 1) % profile.size()];
        const double du = b.u - a.u, dv = b.v - a.v, chord = std::sqrt(du * du + dv * dv);
        if (chord < 1e-12)
        {
            error = "profile has two coincident consecutive vertices (index " + std::to_string(i) + ")";
            return false;
        }
        if (std::abs(a.bulge) < 1e-12)
        {
            SmLine* line = nullptr;
            const SmApiStatus s = SmApiCreateLineSegment(SmPoint3d(a.u, a.v, 0), SmPoint3d(b.u, b.v, 0), line);
            if (s != SM_SUCCESS || !line)
                return Fail(error, "profile line", s);
            curves.Add(line);
            continue;
        }
        // Arc from the bulge: sweep = 4 atan(bulge), centre on the chord's perpendicular bisector.
        const double k = a.bulge, sweep = 4.0 * std::atan(k);
        const double radius = chord * (1.0 + k * k) / (4.0 * std::abs(k));
        const double offset = chord * (1.0 - k * k) / (4.0 * k);  // signed, along the chord's left normal
        const double cu = (a.u + b.u) / 2 - dv / chord * offset, cv = (a.v + b.v) / 2 + du / chord * offset;
        double start = std::atan2(a.v - cv, a.u - cu) * 180.0 / kPi;
        double end = std::atan2(b.v - cv, b.u - cu) * 180.0 / kPi;
        // SmApiCreateArc runs counter-clockwise from start to end: a clockwise arc is built from b to a.
        if (sweep < 0)
            std::swap(start, end);
        while (end <= start)
            end += 360.0;
        SmBSplineCurve* arc = nullptr;
        const SmApiStatus s = SmApiCreateArc(SmPoint3d(cu, cv, 0), radius, start, end, arc);
        if (s != SM_SUCCESS || !arc)
            return Fail(error, "profile arc", s);
        curves.Add(arc);
    }
    return true;
}

void DeleteCurves(SmTArray<SmCurve*>& curves)
{
    for (ULONG i = 0; i < curves.GetSize(); ++i)
        delete curves[i];
    curves.RemoveAll();
}

Solid Check(SmApiStatus status, SmBrep* brep, const char* what, std::string& error)
{
    if (status != SM_SUCCESS || !brep)
    {
        delete brep;
        Fail(error, what, status);
        return Solid();
    }
    return Access::Wrap(brep);
}

// Brep created along +Z from the origin, then aligned with `axis` and moved to `base`.
bool AlignZ(SmBrep* brep, const Vec3& base, const Vec3& axis, std::string& error)
{
    const Vec3 z{0, 0, 1}, a = Unit(axis);
    const Vec3 r = Cross(z, a);
    const double s = Length(r), c = Dot(z, a);
    if (s > 1e-12 || c < 0)
    {
        const double angle = std::atan2(s, c) * 180.0 / kPi;
        const SmApiStatus st = SmApiRotate(brep, SmVector3d(0, 0, 0), s > 1e-12 ? V(r) : SmVector3d(1, 0, 0), angle);
        if (st != SM_SUCCESS)
            return Fail(error, "aligning a primitive", st);
    }
    const SmApiStatus st = SmApiTranslate(brep, V(base));
    return st == SM_SUCCESS || Fail(error, "placing a primitive", st);
}

}  // namespace

Vec3 Plane::Normal() const
{
    return Unit(Cross(xAxis, yAxis));
}

Vec3 Plane::ToWorld(double u, double v) const
{
    return {origin.x + u * xAxis.x + v * yAxis.x, origin.y + u * xAxis.y + v * yAxis.y, origin.z + u * xAxis.z + v * yAxis.z};
}

Solid::Solid() = default;
Solid::~Solid() = default;
Solid::Solid(Solid&&) noexcept = default;
Solid& Solid::operator=(Solid&&) noexcept = default;

bool Solid::Valid() const
{
    return m && m->brep;
}

Solid Solid::Copy() const
{
    SmBrep* copy = nullptr;
    if (!Valid() || SmApiBrepCopy(m->brep, copy) != SM_SUCCESS)
        return Solid();
    return Access::Wrap(copy);
}

void Initialize()
{
    static std::once_flag once;
    std::call_once(once, [] {
        SmApiCreateContext();
        SmApiUsdEnsurePluginRegistered();
    });
}

Solid Extrude(const Plane& plane, const Profile& profile, double distance, std::string& error)
{
    SmTArray<SmCurve*> curves;
    if (!ProfileCurves(profile, curves, error))
    {
        DeleteCurves(curves);
        return Solid();
    }
    SmBrep* brep = nullptr;
    const double d = std::abs(distance);
    const SmApiStatus s = SmApiCreateLinearSweep(curves, SmVector3d(0, 0, distance >= 0 ? 1.0 : -1.0), d, TRUE, brep);
    DeleteCurves(curves);
    Solid solid = Check(s, brep, "extrude", error);
    if (solid.Valid() && !PlaceOnPlane(Access::Get(solid), plane, error))
        return Solid();
    return solid;
}

namespace
{
SmBrep* RevolveCurves(const Profile& profile, double axisU, double axisV, double dirU, double dirV, double fromDeg, double angleDeg, std::string& error)
{
    SmTArray<SmCurve*> curves;
    if (!ProfileCurves(profile, curves, error))
    {
        DeleteCurves(curves);
        return nullptr;
    }
    SmBrep* brep = nullptr;
    SmApiStatus s = SmApiCreateRotationalSweep(curves, SmPoint3d(axisU, axisV, 0), SmVector3d(dirU, dirV, 0), angleDeg, TRUE, brep);
    DeleteCurves(curves);
    if (s == SM_SUCCESS && brep && std::abs(fromDeg) > 1e-12)
        s = SmApiRotate(brep, SmVector3d(axisU, axisV, 0), SmVector3d(dirU, dirV, 0), fromDeg);
    if (s != SM_SUCCESS || !brep)
    {
        delete brep;
        Fail(error, "revolve", s);
        return nullptr;
    }
    return brep;
}
}  // namespace

Solid Revolve(const Plane& plane, const Profile& profile, double axisU, double axisV, double dirU, double dirV, double angleDeg,
              std::string& error)
{
    SmBrep* brep = RevolveCurves(profile, axisU, axisV, dirU, dirV, 0.0, angleDeg, error);
    if (!brep)
        return Solid();
    Solid solid = Access::Wrap(brep);
    if (!PlaceOnPlane(Access::Get(solid), plane, error))
        return Solid();
    return solid;
}
Solid ExtrudeSmooth(const Plane& plane, const std::vector<std::array<double, 2>>& points, double distance, std::string& error)
{
    if (points.size() < 4)
    {
        error = "a smooth profile needs at least 4 points";
        return Solid();
    }
    SmTArray<SmPoint3d> pts;
    for (const auto& p : points)
        pts.Add(SmPoint3d(p[0], p[1], 0));
    pts.Add(SmPoint3d(points[0][0], points[0][1], 0));  // closed
    SmCurve* curve = nullptr;
    const SmApiStatus s = SmApiCreateCurveInterpPoints(pts, curve, SM_CP_CENTRIPETAL);
    if (s != SM_SUCCESS || !curve)
    {
        delete curve;
        Fail(error, "smooth profile", s);
        return Solid();
    }
    SmTArray<SmCurve*> curves;
    curves.Add(curve);
    SmBrep* brep = nullptr;
    const SmApiStatus sw = SmApiCreateLinearSweep(curves, SmVector3d(0, 0, distance >= 0 ? 1.0 : -1.0), std::abs(distance), TRUE, brep);
    DeleteCurves(curves);
    Solid solid = Check(sw, brep, "extrude (smooth profile)", error);
    if (solid.Valid() && !PlaceOnPlane(Access::Get(solid), plane, error))
        return Solid();
    return solid;
}

// Primitives are sketches too: a rectangle or a circle extruded, a half disc or a circle revolved.
Solid Box(const Vec3& corner, const Vec3& size, std::string& error)
{
    Plane base{corner, {1, 0, 0}, {0, 1, 0}};
    return Extrude(base, {{0, 0, 0}, {size.x, 0, 0}, {size.x, size.y, 0}, {0, size.y, 0}}, size.z, error);
}

namespace
{
// Plane through `base` whose normal is `axis`.
Plane PlaneNormalTo(const Vec3& base, const Vec3& axis)
{
    const Vec3 n = Unit(axis);
    const Vec3 helper = std::abs(n.x) < 0.9 ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 x = Unit(Cross(helper, n));
    return Plane{base, x, Cross(n, x)};
}
Profile Circle(double radius)
{
    return {{radius, 0, 1.0}, {-radius, 0, 1.0}};
}
}  // namespace

Solid Cylinder(const Vec3& base, const Vec3& axis, double radius, double height, std::string& error)
{
    return Extrude(PlaneNormalTo(base, axis), Circle(radius), height, error);
}

Solid Sphere(const Vec3& centre, double radius, std::string& error)
{
    // Half disc in the XZ plane revolved about Z.
    Plane xz{centre, {1, 0, 0}, {0, 0, 1}};
    return Revolve(xz, {{0, -radius, 0}, {0, radius, -1.0}}, 0, 0, 0, 1, 360.0, error);
}

Solid Torus(const Vec3& centre, const Vec3& axis, double majorRadius, double minorRadius, std::string& error)
{
    const Plane p = PlaneNormalTo(centre, axis);
    // Section plane spanned by the radial direction and the axis.
    Plane section{centre, p.xAxis, Unit(axis)};
    return Revolve(section, {{majorRadius + minorRadius, 0, 1.0}, {majorRadius - minorRadius, 0, 1.0}}, 0, 0, 0, 1, 360.0, error);
}
Solid HelicalPipe(const Vec3& base, const Vec3& axis, double coilRadius, double height, double turns, double wireRadius, std::string& error)
{
    SmBSplineCurve* helix = nullptr;
    SmApiStatus s = SmApiCreateHelix(SmPoint3d(0, 0, 0), height, coilRadius, coilRadius, turns, TRUE, wireRadius * 0.01, helix);
    if (s != SM_SUCCESS || !helix)
    {
        Fail(error, "helix", s);
        return Solid();
    }
    SmBrep* brep = nullptr;
    s = SmApiCreatePipeSweep(wireRadius, helix, TRUE, brep);
    delete helix;
    Solid solid = Check(s, brep, "helical pipe", error);
    if (solid.Valid() && !AlignZ(Access::Get(solid), base, axis, error))
        return Solid();
    return solid;
}

Solid Pipe(const std::vector<Vec3>& path, double radius, std::string& error)
{
    if (path.size() < 4)
    {
        error = "a pipe path needs at least 4 points";
        return Solid();
    }
    // The pipe sweep is sensitive to the path spline: try the parameterizations, then a path densified
    // with a Catmull-Rom curve through the same points (smoother bends, same ends).
    std::vector<Vec3> dense;
    for (size_t i = 0; i + 1 < path.size(); ++i)
    {
        const Vec3 p0 = path[i == 0 ? 0 : i - 1], p1 = path[i], p2 = path[i + 1], p3 = path[std::min(i + 2, path.size() - 1)];
        for (int k = 0; k < 4; ++k)
        {
            const double t = k / 4.0, t2 = t * t, t3 = t2 * t;
            auto cr = [&](double a, double b, double c, double d) { return 0.5 * (2 * b + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3); };
            dense.push_back({cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y), cr(p0.z, p1.z, p2.z, p3.z)});
        }
    }
    dense.push_back(path.back());
    SmApiStatus s = SM_ERR;
    const std::vector<const std::vector<Vec3>*> candidates = {&path, &dense};
    const SmCurveParameterizationType parameterizations[] = {SM_CP_CHORDLENGTH, SM_CP_CENTRIPETAL, SM_CP_UNIFORM};
    for (const std::vector<Vec3>* candidate : candidates)
        for (const SmCurveParameterizationType parameterization : parameterizations)
        {
            SmTArray<SmPoint3d> points;
            for (const Vec3& p : *candidate)
                points.Add(V(p));
            SmCurve* curve = nullptr;
            s = SmApiCreateCurveInterpPoints(points, curve, parameterization);
            auto* spline = dynamic_cast<SmBSplineCurve*>(curve);
            SmBrep* brep = nullptr;
            if (s == SM_SUCCESS && spline)
                s = SmApiCreatePipeSweep(radius, spline, TRUE, brep);
            delete curve;
            if (s == SM_SUCCESS && brep)
                return Access::Wrap(brep);
            delete brep;
        }
    Fail(error, "pipe (the bends may be tighter than the pipe radius)", s);
    return Solid();
}

namespace
{

// What makes SMLib's booleans fail (measured: a 50 um ring and mark fail joined flush or overlapping; from about
// 0.15 mm they work).
constexpr const char* kBooleanHint =
    "The kernel fails on solids that touch exactly along faces (on the same plane, or a face lying on another) and on "
    "features finer than about 0.15 mm (ring, marks, walls). Overlap the solids clearly instead of letting them touch, "
    "or make the fine features larger, or keep them as separate parts";

// SMLib's merge of coincident faces can read outside its arrays (an access violation in SmApiBooleanUnion, e.g. a ring
// and markers sharing their top face): caught here, so that the boolean fails instead of the application.
SmApiStatus GuardedBoolean(BooleanOp op, SmBrep* a, SmBrep* b, SmBrep** result, bool* crashed)
{
    __try
    {
        return op == BooleanOp::Union      ? SmApiBooleanUnion(a, b, *result)
               : op == BooleanOp::Subtract ? SmApiBooleanDifference(a, b, *result)
                                           : SmApiBooleanIntersection(a, b, *result);
    }
    __except (GetExceptionCode() == 0xC0000005 /* access violation */ ? 1 /* handle */ : 0 /* search on */)
    {
        *crashed = true;
        return SM_ERR;
    }
}

}  // namespace

bool Boolean(Solid& a, Solid&& b, BooleanOp op, std::string& error)
{
    SmBrep* pa = Access::Get(a);
    SmBrep* pb = Access::Get(b);
    if (!pa || !pb)
    {
        error = "boolean on an empty solid";
        return false;
    }
    SmBrep* result = nullptr;
    bool crashed = false;
    const SmApiStatus s = GuardedBoolean(op, pa, pb, &result, &crashed);
    if (crashed)
    {
        // Both operands may be half rewritten: never touched again (leaked rather than freed).
        Access::Release(a);
        Access::Release(b);
        error = std::string(op == BooleanOp::Union ? "union" : op == BooleanOp::Subtract ? "subtract" : "intersect") +
                " failed inside the geometry kernel (it stopped on bad memory, caught). " + kBooleanHint;
        return false;
    }
    if (s != SM_SUCCESS || result != pa)
    {
        Fail(error, op == BooleanOp::Union ? "union" : op == BooleanOp::Subtract ? "subtract" : "intersect", s);
        error += ". " + std::string(kBooleanHint);
        return false;
    }
    Access::Release(b);  // deleted by SMLib on success
    return true;
}

bool Rotate(Solid& solid, const Vec3& pivot, const Vec3& axis, double angleDeg, std::string& error)
{
    const SmApiStatus s = SmApiRotate(Access::Get(solid), V(pivot), V(axis), angleDeg);
    return s == SM_SUCCESS || Fail(error, "rotate", s);
}

bool Translate(Solid& solid, const Vec3& offset, std::string& error)
{
    const SmApiStatus s = SmApiTranslate(Access::Get(solid), V(offset));
    return s == SM_SUCCESS || Fail(error, "translate", s);
}

Solid KeepBelow(const Solid& solid, const Vec3& point, const Vec3& normal, std::string& error)
{
    Vec3 lo, hi;
    if (!BoundingBox(solid, lo, hi))
    {
        error = "section of an empty solid";
        return Solid();
    }
    // A cube larger than the solid with one face on the plane, on the +normal side. An edge lying exactly
    // in the plane (the seam of a profile revolved from that plane) defeats the boolean: then the plane
    // moves by 10 micrometres, invisible in the view.
    const Vec3 n = Unit(normal);
    // Tight cutter: the box just covers the solid's bounds on the +normal side.
    const double diag = Length({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z});
    const double size = 2.2 * (diag + Length({point.x - (lo.x + hi.x) / 2, point.y - (lo.y + hi.y) / 2, point.z - (lo.z + hi.z) / 2})) + 1e-3;
    for (const double shift : {0.0, 1e-5, -1e-5, 3e-5})
    {
        const Vec3 at{point.x + n.x * shift, point.y + n.y * shift, point.z + n.z * shift};
        Solid cutter = Box({-size / 2, -size / 2, 0}, {size, size, size}, error);
        if (!cutter.Valid() || !AlignZ(Access::Get(cutter), at, n, error))
            return Solid();
        Solid result = solid.Copy();
        if (!Boolean(result, std::move(cutter), BooleanOp::Subtract, error))
            continue;
        error.clear();
        return FaceCount(result) == 0 ? Solid() : std::move(result);
    }
    return Solid();
}

bool ComputeMass(const Solid& solid, double density, MassProperties& out, std::string& error)
{
    if (!solid.Valid())
    {
        error = "mass of an empty solid";
        return false;
    }
    // On a copy: SMLib's mass integration changes the brep, and every later tessellation of it (and of its copies)
    // comes out coarse or broken (a hairspring's section lost 7 points in 8; its display mesh had -485 % volume).
    const Solid scratch = solid.Copy();
    const SmBrep* brep = Access::Get(scratch);
    SmPoint3d centroid;
    SmVector3d moments, products;
    double area = 0, volume = 0, mass = 0;
    // First pass for the centroid, second for the inertia about it.
    SmApiStatus s = SmApiBrepComputeMassProperties(brep, 1e-4, density, SmPoint3d(0, 0, 0), area, volume, mass, centroid, moments, products);
    if (s != SM_SUCCESS)
        return Fail(error, "mass properties", s);
    s = SmApiBrepComputeMassProperties(brep, 1e-4, density, centroid, area, volume, mass, centroid, moments, products);
    if (s != SM_SUCCESS)
        return Fail(error, "mass properties", s);
    out.volume = volume;
    out.area = area;
    out.mass = mass;
    out.centroid = FromSm(centroid);
    // Products come as positive integrals; tensor off-diagonals are their negatives.
    out.inertia = {moments[0], moments[1], moments[2], -products[0], -products[1], -products[2]};
    return true;
}

bool BoundingBox(const Solid& solid, Vec3& minimum, Vec3& maximum)
{
    SmPoint3d lo, hi;
    if (!solid.Valid() || SmApiBrepBoundingBox(Access::Get(solid), TRUE, lo, hi) != SM_SUCCESS)
        return false;
    minimum = FromSm(lo);
    maximum = FromSm(hi);
    return true;
}

bool IsManifoldSolid(const Solid& solid)
{
    SmBoolean manifold = FALSE;
    return solid.Valid() && SmApiBrepIsManifoldSolid(Access::Get(solid), manifold) == SM_SUCCESS && manifold;
}

int FaceCount(const Solid& solid)
{
    SmTArray<SmFace*> faces;
    if (!solid.Valid() || SmApiGetFaces(Access::Get(solid), faces) != SM_SUCCESS)
        return 0;
    return int(faces.GetSize());
}

bool Tessellate(const Solid& solid, double chordTolerance, double angleDeg, Mesh& out, std::string& error)
{
    SmBrep* brep = Access::Get(solid);
    if (!brep)
    {
        error = "tessellating an empty solid";
        return false;
    }
    SmTessellationParams params;
    params.dChordHeightTol = chordTolerance;
    params.dSurfaceAngleTolDeg = angleDeg;
    params.dCurveAngleTolDeg = angleDeg;
    SmPolyBrep* raw = nullptr;
    const SmApiStatus s = SmApiTessellate(brep, raw, params);
    if (s != SM_SUCCESS || !raw)
        return Fail(error, "tessellate", s);
    SMU_BrepConvert::TessellatedMeshPtr mesh(raw);
    pxr::VtArray<pxr::GfVec3f> extent, points, normals;
    pxr::VtIntArray counts, indices, normalIndices;
    pxr::TfToken interpolation, scheme;
    if (SMU_BrepConvert::GetMeshData(*mesh, extent, points, counts, indices, normals, normalIndices, interpolation, scheme) != SM_SUCCESS)
    {
        error = "tessellation data could not be extracted";
        return false;
    }
    // The tessellation shares points between faces and carries no normals: normals come from the
    // triangles with a crease angle (smooth across a curved face, sharp at the edges between faces).
    const size_t triangleCount = std::count_if(counts.begin(), counts.end(), [](int c) { return c >= 3; });
    std::vector<std::array<int, 3>> tris;
    tris.reserve(triangleCount);
    size_t corner = 0;
    for (const int count : counts)
    {
        for (int k = 1; k + 1 < count; ++k)
            tris.push_back({indices[corner], indices[corner + size_t(k)], indices[corner + size_t(k) + 1]});
        corner += size_t(count);
    }
    std::vector<pxr::GfVec3d> faceNormal(tris.size());  // area weighted
    std::vector<std::vector<int>> around(points.size());
    for (size_t t = 0; t < tris.size(); ++t)
    {
        const pxr::GfVec3d a(points[size_t(tris[t][0])]), b(points[size_t(tris[t][1])]), c(points[size_t(tris[t][2])]);
        faceNormal[t] = pxr::GfCross(b - a, c - a);
        for (const int v : tris[t])
            around[size_t(v)].push_back(int(t));
    }
    const double creaseCos = std::cos(35.0 * kPi / 180.0);
    out = Mesh();
    out.points.reserve(tris.size() * 3);
    for (size_t t = 0; t < tris.size(); ++t)
    {
        const pxr::GfVec3d own = faceNormal[t].GetNormalized();
        const int first = int(out.points.size());
        for (const int v : tris[t])
        {
            pxr::GfVec3d n(0.0);
            for (const int u : around[size_t(v)])
                if (pxr::GfDot(faceNormal[size_t(u)].GetNormalized(), own) > creaseCos)
                    n += faceNormal[size_t(u)];
            n = n.GetLength() > 0 ? n.GetNormalized() : own;
            const pxr::GfVec3f& p = points[size_t(v)];
            out.points.push_back({p[0], p[1], p[2]});
            out.normals.push_back({float(n[0]), float(n[1]), float(n[2])});
        }
        out.triangles.push_back({first, first + 1, first + 2});
    }
    return true;
}
bool WriteToPrim(const std::vector<const Solid*>& solids, pxr::UsdPrim& brepArray, std::string& error)
{
    std::vector<SmBrep*> breps;
    for (const Solid* s : solids)
        if (s && s->Valid())
            breps.push_back(Access::Get(*s));
    if (breps.empty())
    {
        error = "nothing to write";
        return false;
    }
    const SmStatus s = SMU_BrepConvert::BrepAppend_SMLibToUsd(breps, brepArray, true, true);
    if (s != SM_SUCCESS)
    {
        error = "writing the BrepArray failed (status " + std::to_string(int(s)) + ")";
        return false;
    }
    return true;
}

std::vector<Solid> ReadFromPrim(const pxr::UsdPrim& brepArray, std::string& error)
{
    std::vector<Solid> solids;
    std::vector<SmBrep*> breps;
    const SmStatus s = SMU_BrepConvert::BrepMove_UsdToSMLib(*SmApiGetOrCreateContext(), pxr::UsdGeomGprim(brepArray), breps, FALSE);
    if (s != SM_SUCCESS)
    {
        error = "reading the BrepArray failed (status " + std::to_string(int(s)) + ")";
        for (SmBrep* b : breps)
            delete b;
        return solids;
    }
    for (SmBrep* b : breps)
        solids.push_back(Access::Wrap(b));
    return solids;
}

}  // namespace aiad::kernel
