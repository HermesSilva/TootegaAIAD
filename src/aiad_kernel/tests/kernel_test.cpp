// Kernel facade against analytic answers. What failure would this catch?
// - a sketch placed on the wrong plane or an arc bulge drawn on the wrong side (wrong volume/bounds);
// - revolve/extrude/boolean/section results that are not closed solids;
// - mass properties (the physics comes from them) off by a factor or about the wrong point;
// - B-rep geometry lost on the way through a USD BrepArray prim.
#include "Kernel.h"

#include <pxr/base/plug/registry.h>
#include <pxr/usd/usd/stage.h>

#include <windows.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <numbers>

using namespace aiad::kernel;

static int g_failures = 0;
#define CHECK(c)                                                         \
    do                                                                   \
    {                                                                    \
        if (!(c))                                                        \
        {                                                                \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);     \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

static bool Near(double a, double b, double relative)
{
    return std::abs(a - b) <= relative * std::max(std::abs(b), 1e-12);
}

static double Volume(const Solid& s)
{
    MassProperties mp;
    std::string error;
    return ComputeMass(s, 1.0, mp, error) ? mp.volume : -1.0;
}

int main()
{
    _putenv_s("OMNISOLID_PLUGIN_PATH", AIAD_OMNISOLID_DIR);  // read with getenv: the CRT copy
    // Static OpenUSD reads PXR_PLUGINPATH_NAME at process start: register the folder explicitly.
    pxr::PlugRegistry::GetInstance().RegisterPlugins(std::string(AIAD_USD_PLUGIN_DIR));
    Initialize();
    const double pi = std::numbers::pi;
    std::string error;

    // Box: volume and mass.
    Solid box = Box({0, 0, 0}, {0.02, 0.03, 0.04}, error);
    CHECK(box.Valid() && IsManifoldSolid(box));
    MassProperties mp;
    CHECK(ComputeMass(box, 7850.0, mp, error) && Near(mp.volume, 2.4e-5, 1e-6) && Near(mp.mass, 0.1884, 1e-6));
    CHECK(Near(mp.centroid.x, 0.01, 1e-6) && Near(mp.centroid.z, 0.02, 1e-6));
    // Ixx about the centroid = m (b^2 + c^2) / 12.
    CHECK(Near(mp.inertia[0], 0.1884 * (0.03 * 0.03 + 0.04 * 0.04) / 12.0, 1e-3));

    // Extruded slot (two half-circle ends: bulge +1 and +1): area = L*2r + pi r^2.
    const double L = 0.04, r = 0.01, h = 0.005;
    Profile slot = {{0, -r, 0}, {L, -r, 1.0}, {L, r, 0}, {0, r, 1.0}};
    Solid slotSolid = Extrude(Plane{}, slot, h, error);
    CHECK(slotSolid.Valid() && IsManifoldSolid(slotSolid));
    CHECK(Near(Volume(slotSolid), (L * 2 * r + pi * r * r) * h, 1e-4));
    Vec3 lo, hi;
    CHECK(BoundingBox(slotSolid, lo, hi) && Near(lo.x, -r, 1e-3) && Near(hi.x, L + r, 1e-3) && Near(hi.z, h, 1e-3));

    // A clockwise bite (bulge -1) takes a half disc out of the rectangle instead.
    Profile bite = {{0, -r, 0}, {L, -r, 0}, {L, r, 0}, {0, r, -1.0}};
    CHECK(Near(Volume(Extrude(Plane{}, bite, h, error)), (L * 2 * r - pi * r * r / 2) * h, 1e-4));

    // Sketch on another plane: XZ plane moved to y = 0.1, extruded along its normal (-Y).
    Plane xz{{0, 0.1, 0}, {1, 0, 0}, {0, 0, 1}};
    Solid onXz = Extrude(xz, {{0, 0, 0}, {0.01, 0, 0}, {0.01, 0.02, 0}, {0, 0.02, 0}}, 0.03, error);
    CHECK(onXz.Valid() && BoundingBox(onXz, lo, hi) && Near(hi.y, 0.1, 1e-3) && Near(lo.y, 0.07, 1e-3) && Near(hi.z, 0.02, 1e-3));

    // Revolve a rectangle about the sketch's v axis: a tube.
    const double r1 = 0.02, r2 = 0.03, th = 0.05;
    Solid tube = Revolve(Plane{}, {{r1, 0, 0}, {r2, 0, 0}, {r2, th, 0}, {r1, th, 0}}, 0, 0, 0, 1, 360.0, error);
    CHECK(tube.Valid() && IsManifoldSolid(tube));
    CHECK(Near(Volume(tube), pi * (r2 * r2 - r1 * r1) * th, 1e-4));

    // Cylinder along X: mass properties about its own axis (Ixx = m r^2 / 2).
    Solid cylinder = Cylinder({0, 0, 0}, {1, 0, 0}, 0.01, 0.1, error);
    CHECK(ComputeMass(cylinder, 2700.0, mp, error) && Near(mp.inertia[0], mp.mass * 0.01 * 0.01 / 2, 2e-3) && Near(mp.centroid.x, 0.05, 1e-4));

    // Boolean: a through hole.
    Solid plate = Box({0, 0, 0}, {0.05, 0.05, 0.01}, error);
    CHECK(Boolean(plate, Cylinder({0.025, 0.025, -0.01}, {0, 0, 1}, 0.005, 0.03, error), BooleanOp::Subtract, error));
    CHECK(IsManifoldSolid(plate) && Near(Volume(plate), 0.05 * 0.05 * 0.01 - pi * 0.005 * 0.005 * 0.01, 1e-4));

    // A watch dial's track: two 50 um rings and a mark flush between them. SMLib crashed inside this union (access
    // violation in its coincident-face merge): it must come back as a failed boolean with its explanation.
    {
        Plane xzPlane{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        auto ring = [&](double r0, double r1) { return Revolve(xzPlane, {{r0, -4e-5, 0}, {r1, -4e-5, 0}, {r1, 0, 0}, {r0, 0, 0}}, 0, 0, 0, 1, 360.0, error); };
        Solid track = ring(0.01685, 0.0169);
        CHECK(Boolean(track, ring(0.0173, 0.01735), BooleanOp::Union, error));
        std::string why;
        CHECK(!Boolean(track, Box({0.0169, -2.5e-5, -4e-5}, {4e-4, 5e-5, 4e-5}, error), BooleanOp::Union, why) && why.find("touch exactly") != std::string::npos);
    }

    // Section: half of the plate below y = 0.025 (the hole is cut in two).
    Solid half = KeepBelow(plate, {0, 0.025, 0}, {0, 1, 0}, error);
    CHECK(half.Valid() && IsManifoldSolid(half) && Near(Volume(half), Volume(plate) / 2, 1e-3));
    CHECK(Volume(plate) > 0 && FaceCount(plate) > 0);  // the source is untouched

    // Section of revolved housings whose seam lies on the cut plane (profile drawn on that plane).
    for (const char* name : {"XZ", "YX"})
    {
        const bool xz = name[0] == 'X';
        Plane plane = xz ? Plane{{0, 0, 0}, {1, 0, 0}, {0, 0, 1}} : Plane{{0, 0, 0}, {0, 1, 0}, {1, 0, 0}};
        Solid ring = Revolve(plane, {{0.03, 0, 0}, {0.04, 0, 0}, {0.04, 0.05, 0}, {0.03, 0.05, 0}}, 0, 0, 0, 1, 360.0, error);
        std::string e;
        Solid back = KeepBelow(ring, {0, 0, 0}, {0, -1, 0}, e);
        if (!back.Valid())
            std::printf("section of a ring revolved on %s failed: %s\n", name, e.c_str());
        CHECK(back.Valid() && Near(Volume(back), Volume(ring) / 2, 1e-3));
    }

    // Spring: wire volume = pi a^2 * helix length.
    const double R = 0.012, H = 0.04, turns = 6, a = 0.0015;
    Solid spring = HelicalPipe({0, 0, 0}, {0, 0, 1}, R, H, turns, a, error);
    const double helixLength = std::sqrt(std::pow(2 * pi * R * turns, 2) + H * H);
    CHECK(spring.Valid() && Near(Volume(spring), pi * a * a * helixLength, 2e-2));

    // Tessellation: normals point out of the material and agree with the triangle winding (a renderer
    // shades with them; inverted ones make the part black).
    Mesh mesh;
    CHECK(Tessellate(tube, 1e-4, 10.0, mesh, error) && mesh.triangles.size() > 50 && mesh.points.size() == mesh.normals.size());
    for (const Solid* s : {&box, &tube, &plate})
    {
        Mesh m;
        CHECK(Tessellate(*s, 1e-4, 10.0, m, error));
        MassProperties c;
        ComputeMass(*s, 1.0, c, error);
        int outward = 0, agree = 0, total = 0;
        for (const auto& t : m.triangles)
        {
            const auto& a = m.points[size_t(t[0])];
            const auto& b = m.points[size_t(t[1])];
            const auto& cc = m.points[size_t(t[2])];
            const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = cc[0] - a[0], vy = cc[1] - a[1], vz = cc[2] - a[2];
            const double gx = uy * vz - uz * vy, gy = uz * vx - ux * vz, gz = ux * vy - uy * vx;
            const auto& n = m.normals[size_t(t[0])];
            agree += gx * n[0] + gy * n[1] + gz * n[2] > 0;
            // Outward for a box: away from the centroid along the face normal.
            outward += (a[0] - c.centroid.x) * n[0] + (a[1] - c.centroid.y) * n[1] + (a[2] - c.centroid.z) * n[2] > 0;
            ++total;
        }
        if (agree != total) std::printf("normals: agree %d outward %d of %d\n", agree, outward, total);
        CHECK(agree == total);
        if (s == &box)
            CHECK(outward == total);
    }

    // Section view on meshes: the caps close the cut exactly (rectangles, an annulus with its hole).
    {
        Mesh tubeMesh, kept, cap;
        CHECK(Tessellate(tube, 2e-5, 5.0, tubeMesh, error));
        SectionMesh(tubeMesh, {0, 0, 0}, {0, 0, -1}, kept, cap);  // through the axis (Y): two rectangles
        CHECK(Near(MeshArea(cap), 2 * (r2 - r1) * th, 2e-3) && !kept.triangles.empty());
        SectionMesh(tubeMesh, {0, th / 2, 0}, {0, 1, 0}, kept, cap);  // across the axis: an annulus (hole bridged)
        CHECK(Near(MeshArea(cap), pi * (r2 * r2 - r1 * r1), 5e-3));
        for (const auto& nn : cap.normals)
            CHECK(std::abs(nn[1] - 1.0f) < 1e-6f);  // facing the removed side
        Mesh plateMesh;
        CHECK(Tessellate(plate, 2e-5, 5.0, plateMesh, error));
        SectionMesh(plateMesh, {0, 0.025, 0}, {0, 1, 0}, kept, cap);  // through the hole: the bar minus the hole's width
        CHECK(Near(MeshArea(cap), 0.05 * 0.01 - 0.01 * 0.01, 3e-3));
    }

    // Through a USD BrepArray prim and back.
    pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
    pxr::UsdPrim prim = stage->DefinePrim(pxr::SdfPath("/Part/Shape"), pxr::TfToken("BrepArray"));
    CHECK(WriteToPrim({&plate, &tube}, prim, error));
    std::vector<Solid> back = ReadFromPrim(prim, error);
    CHECK(back.size() == 2);
    if (back.size() == 2)
        CHECK(Near(Volume(back[0]), Volume(plate), 1e-4) && Near(Volume(back[1]), Volume(tube), 1e-4));
    if (!error.empty())
        std::printf("last error: %s\n", error.c_str());

    std::printf(g_failures ? "kernel_test: %d failure(s)\n" : "kernel_test: ok\n", g_failures);
    return g_failures ? 1 : 0;
}