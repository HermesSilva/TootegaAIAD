// Spike S5: USD BRep solid -> OCCT .brep text (OCCT_BREP_EXPORT) -> OCCT healing -> Netgen tetrahedra
// (static Netgen C++ API + OCCT) -> MFEM mesh with boundary attributes = B-rep faces. Everything
// statically linked.
// Checks: one boundary attribute per B-rep face, and mesh volume == exact solid volume (SMLib).
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>

#include "OcctExport.h"
#include "SmApiBrep.h"
#include "SmApiGeneral.h"
#include "SmApiQueries.h"
#include "SmApiUsd.h"
#include "UsdBrepStageUtils.h"

#include <mfem.hpp>

#include <meshing.hpp> // netgen C++ API
#include <occgeom.hpp> // netgen OCC geometry

#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <ShapeFix_Shape.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

static double TetVolume(const netgen::Point3d& a, const netgen::Point3d& b, const netgen::Point3d& c,
                        const netgen::Point3d& d)
{
    const double u[3] = { b.X() - a.X(), b.Y() - a.Y(), b.Z() - a.Z() };
    const double v[3] = { c.X() - a.X(), c.Y() - a.Y(), c.Z() - a.Z() };
    const double w[3] = { d.X() - a.X(), d.Y() - a.Y(), d.Z() - a.Z() };
    return std::abs(u[0] * (v[1] * w[2] - v[2] * w[1]) - u[1] * (v[0] * w[2] - v[2] * w[0]) +
                    u[2] * (v[0] * w[1] - v[1] * w[0])) / 6.0;
}

// Signed volume of every shell of the shape: a valid solid has one positive outer shell and
// negative inner shells (cavities).
static void PrintShells(const char* label, const TopoDS_Shape& shape)
{
    std::printf("%s:", label);
    for (TopExp_Explorer ex(shape, TopAbs_SHELL); ex.More(); ex.Next())
    {
        GProp_GProps props;
        BRepGProp::VolumeProperties(ex.Current(), props);
        std::printf(" shell %+.9g", props.Mass());
    }
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    std::printf(" | total %.9g\n", props.Mass());
}

static int Run(int argc, char** argv);

int main(int argc, char** argv)
{
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::printf("[FAIL] exception: %s\n", e.what());
    }
    catch (...)
    {
        std::printf("[FAIL] unknown exception\n");
    }
    return 1;
}

static int Run(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5)
    {
        std::printf("usage: s5_netgen <omniSolid/resources dir> <asset.usda> <work dir> <expected faces, 0 = any> [volume tolerance]\n");
        return 2;
    }
    const std::string omniSolid = argv[1];
    const std::string asset = argv[2];
    const std::string work = argv[3];
    const int expectedFaces = std::atoi(argv[4]);

    PlugRegistry::GetInstance().RegisterPlugins(omniSolid);

    // 1) Exact volume from the kernel.
    std::vector<SmBrep*> breps;
    if (SmApiUsdImportBreps(asset.c_str(), breps, TRUE) != SM_SUCCESS || breps.empty())
    {
        std::printf("[FAIL] SmApiUsdImportBreps\n");
        return 1;
    }
    double exactVolume = 0.0;
    if (SmApiBrepComputeVolume(breps[0], 1.0e-8, exactVolume) != SM_SUCCESS)
    {
        std::printf("[FAIL] SmApiBrepComputeVolume\n");
        return 1;
    }
    std::printf("SMLib: %zu brep(s), exact volume %.9g\n", breps.size(), exactVolume);
    for (SmBrep* b : breps)
        delete b; // SMLib breps are plain heap objects owned by the caller
    breps.clear();

    // 2) BrepArray -> OCCT ASCII .brep (no OCCT linked by USD BRep).
    std::vector<std::pair<SdfPath, std::unique_ptr<UsdBrepData::UsdBrepArrayData>>> arrays;
    if (!UsdBrep::LoadAllBrepArrays(asset, arrays) || arrays.empty())
    {
        std::printf("[FAIL] LoadAllBrepArrays\n");
        return 1;
    }
    std::string brepText, error;
    if (!occt::ExportBrepArrayToText(*arrays[0].second, brepText, error))
    {
        std::printf("[FAIL] ExportBrepArrayToText: %s\n", error.c_str());
        return 1;
    }
    std::ofstream(work + "/model.brep", std::ios::binary) << brepText;
    std::printf("OCCT .brep: %zu bytes\n", brepText.size());

    // 3) OCCT: read and heal. The exported .brep has no p-curves (Curve2ds 0) and the inner shells
    //    (cavities) come out with the outer-shell orientation; ShapeFix rebuilds the p-curves and
    //    re-orients the shells, otherwise Netgen fills the cavities.
    TopoDS_Shape shape;
    {
        std::istringstream in(brepText);
        BRep_Builder builder;
        BRepTools::Read(shape, in, builder);
    }
    if (shape.IsNull())
    {
        std::printf("[FAIL] BRepTools::Read\n");
        return 1;
    }
    PrintShells("OCCT as exported", shape);
    ShapeFix_Shape fix(shape);
    fix.Perform();
    shape = fix.Shape();
    PrintShells("OCCT healed     ", shape);
    TopTools_IndexedMapOfShape faceMap;
    TopExp::MapShapes(shape, TopAbs_FACE, faceMap);
    const int brepFaces = faceMap.Extent();
    std::printf("OCCT faces: %d (expected %d)\n", brepFaces, expectedFaces);

    // 4) Netgen tetrahedral mesh (C++ API; nglib's C wrapper corrupts the heap on Ng_DeleteMesh).
    netgen::mycout = &std::cout;
    netgen::myerr = &std::cerr;
    auto geometry = std::make_shared<netgen::OCCGeometry>(shape);
    const char* maxhFactor = std::getenv("S5_MAXH"); // mesh size relative to cbrt(volume)
    const double maxh = (maxhFactor ? std::atof(maxhFactor) : 0.1) * std::cbrt(exactVolume);
    std::shared_ptr<netgen::Mesh> ngMesh;
    // Netgen does not propagate a failed face (MeshSurface only prints a warning) and then crashes
    // in the volume mesher, so stop after the surface, check that every face got triangles, and
    // retry with a slightly different size when one did not.
    for (int attempt = 0; attempt < 4 && !ngMesh; ++attempt)
    {
        auto candidate = std::make_shared<netgen::Mesh>();
        candidate->SetGeometry(geometry);
        netgen::MeshingParameters mp;
        mp.maxh = maxh * (1.0 - 0.07 * attempt);
        mp.perfstepsend = netgen::MESHCONST_OPTSURFACE;
        geometry->GenerateMesh(candidate, mp);
        // A failed face can still leave partial triangles, so check the surface is watertight:
        // every triangle edge shared by exactly two triangles.
        std::map<std::pair<int, int>, int> edgeUse;
        for (int i = 0; i < int(candidate->GetNSE()); ++i)
        {
            const netgen::Element2d& el = (*candidate)[netgen::SurfaceElementIndex(i)];
            for (int k = 0; k < 3; ++k)
            {
                const int a = int(el[k]), b = int(el[(k + 1) % 3]);
                ++edgeUse[{ std::min(a, b), std::max(a, b) }];
            }
        }
        size_t openEdges = 0;
        for (const auto& [edge, count] : edgeUse)
            openEdges += count != 2;
        if (candidate->GetNSE() == 0 || openEdges != 0)
        {
            std::printf("Netgen: attempt %d (maxh %.4g): surface not watertight (%zu bad edges), retrying\n", attempt,
                        mp.maxh, openEdges);
            continue;
        }
        mp.perfstepsstart = netgen::MESHCONST_MESHVOLUME;
        mp.perfstepsend = netgen::MESHCONST_OPTVOLUME;
        if (geometry->GenerateMesh(candidate, mp) == 0 && candidate->GetNE() > 0)
            ngMesh = candidate;
        else
            std::printf("Netgen: attempt %d (maxh %.4g): volume meshing failed, retrying\n", attempt, mp.maxh);
    }
    if (!ngMesh)
    {
        std::printf("[FAIL] Netgen meshing\n");
        return 1;
    }
    const int np = int(ngMesh->GetNP()), ne = int(ngMesh->GetNE()), nse = int(ngMesh->GetNSE());
    std::printf("Netgen: %d points, %d tetrahedra, %d boundary triangles, %d face descriptors\n", np, ne, nse,
                int(ngMesh->GetNFD()));

    // 5) MFEM mesh in memory: attribute = Netgen domain (solid), boundary attribute = B-rep face
    //    (face descriptor surface number = 1-based OCC face map index).
    mfem::Mesh mm(3, np, ne, nse);
    for (int i = 0; i < np; ++i)
    {
        const netgen::Point3d& p = (*ngMesh)[netgen::PointIndex(i + netgen::PointIndex::BASE)];
        const double xyz[3] = { p.X(), p.Y(), p.Z() };
        mm.AddVertex(xyz);
    }
    auto vertex = [](netgen::PointIndex pi) { return int(pi - netgen::PointIndex::BASE); };
    double meshVolume = 0.0;
    for (int i = 0; i < ne; ++i)
    {
        const netgen::Element& el = (*ngMesh)[netgen::ElementIndex(i)];
        int t[4] = { vertex(el[0]), vertex(el[1]), vertex(el[2]), vertex(el[3]) };
        mm.AddTet(t, el.GetIndex());
        meshVolume += TetVolume((*ngMesh)[el[0]], (*ngMesh)[el[1]], (*ngMesh)[el[2]], (*ngMesh)[el[3]]);
    }
    std::set<int> attrs;
    for (int i = 0; i < nse; ++i)
    {
        const netgen::Element2d& el = (*ngMesh)[netgen::SurfaceElementIndex(i)];
        const int face = ngMesh->GetFaceDescriptor(el.GetIndex()).SurfNr();
        int tri[3] = { vertex(el[0]), vertex(el[1]), vertex(el[2]) };
        mm.AddBdrTriangle(tri, face);
        attrs.insert(face);
    }
    mm.FinalizeTetMesh(1, 1, true);
    std::printf("MFEM: %d elements, %d boundary elements, %zu distinct boundary attributes (max %d)\n",
                mm.GetNE(), mm.GetNBE(), attrs.size(), mm.bdr_attributes.Size() ? mm.bdr_attributes.Max() : 0);
    double mfemVolume = 0.0;
    for (int e = 0; e < mm.GetNE(); ++e)
        mfemVolume += mm.GetElementVolume(e);

    const double relErr = std::abs(meshVolume - exactVolume) / exactVolume;
    const double relErrMfem = std::abs(mfemVolume - exactVolume) / exactVolume;
    std::printf("volume: mesh %.9g, MFEM %.9g, exact %.9g -> rel. error %.2e / %.2e\n", meshVolume, mfemVolume,
                exactVolume, relErr, relErrMfem);

    std::ofstream out(work + "/model.mesh");
    mm.Print(out); // MFEM native format, readable by MFEM/GLVis/ParaView converters

    // Planar faces mesh exactly; curved faces (the spherical cavity) leave a chord error.
    const double tolerance = std::atof(argc > 5 ? argv[5] : "1e-6");
    const bool ok = int(attrs.size()) == brepFaces && (expectedFaces <= 0 || brepFaces == expectedFaces) && relErr < tolerance && relErrMfem < tolerance;
    std::printf("%s (volume tolerance %.1e)\n", ok ? "ALL PASS" : "FAILURES", tolerance);
    return ok ? 0 : 1;
}
