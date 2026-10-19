// Spike S3a: static OpenUSD 25.11 + static USD BRep + hdUsdBrep linked into one executable.
// Opens a BrepArray asset and checks that Hydra (scene indices only, no renderer) produces
// tessellated meshes through the hdUsdBrep adapter/procedural.
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

#include "diag.inc"

static void InspectProcedural(const HdSceneIndexBaseRefPtr& output, const SdfPath& procPath)
{
    const HdSceneIndexPrim pp = output->GetPrim(procPath);
    std::printf("procedural prim type: '%s'\n", pp.primType.GetText());
    if (pp.dataSource)
        for (const TfToken& n : pp.dataSource->GetNames())
            std::printf("  ds name: %s\n", n.GetText());
    HdPrimvarsSchema pv = HdPrimvarsSchema::GetFromParent(pp.dataSource);
    if (HdSampledDataSourceHandle t = pv.GetPrimvar(TfToken("hdGp:proceduralType")).GetPrimvarValue())
        std::printf("  hdGp:proceduralType = %s\n", TfStringify(t->GetValue(0.0f)).c_str());
    else
        std::printf("  hdGp:proceduralType = <none>\n");
    for (const SdfPath& c : output->GetChildPrimPaths(procPath))
        std::printf("  child: %s\n", c.GetText());

    // What did the adapter tessellate?
    HdContainerDataSourceHandle tess =
        HdContainerDataSource::Cast(pp.dataSource ? pp.dataSource->Get(TfToken("usdBrepTessellatorData")) : nullptr);
    if (!tess)
    {
        std::printf("  usdBrepTessellatorData: <not a container>\n");
        return;
    }
    const TfTokenVector entries = tess->GetNames();
    std::printf("  usdBrepTessellatorData entries: %zu\n", entries.size());
    for (const TfToken& e : entries)
    {
        HdContainerDataSourceHandle brep = HdContainerDataSource::Cast(tess->Get(e));
        HdContainerDataSourceHandle mesh = brep ? HdContainerDataSource::Cast(brep->Get(TfToken("mesh"))) : nullptr;
        size_t n = 0;
        if (mesh)
            if (auto pts = HdSampledDataSource::Cast(mesh->Get(TfToken("points"))))
            {
                const VtValue v = pts->GetValue(0.0f);
                n = v.IsArrayValued() ? v.GetArraySize() : 0;
            }
        std::printf("    entry %s: mesh=%s points=%zu\n", e.GetText(), mesh ? "yes" : "no", n);
    }
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3)
    {
        std::printf("usage: s3a_hydra_brep <plugins-dir> <asset.usda>\n");
        return 2;
    }
    const std::string pluginsDir = argv[1];
    const std::string asset = argv[2];

    // Plugins compiled into this executable (hdUsdBrep) and the codeless omniSolid schema.
    PlugRegistry::GetInstance().RegisterPlugins(
        std::vector<std::string>{ pluginsDir + "/hdUsdBrep", pluginsDir + "/omniSolid/resources" });

    UsdStageRefPtr stage = UsdStage::Open(asset);
    if (!stage)
    {
        std::printf("[FAIL] cannot open %s\n", asset.c_str());
        return 1;
    }
    int brepPrims = 0;
    SdfPath procPath;
    for (const UsdPrim& prim : stage->Traverse())
        if (prim.GetTypeName() == TfToken("BrepArray"))
        {
            ++brepPrims;
            procPath = prim.GetPath();
        }
    std::printf("BrepArray prims in stage: %d\n", brepPrims);

    UsdImagingCreateSceneIndicesInfo info;
    UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr output =
        HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer("HdAiad",
                                                                               indices.finalSceneIndex);
    // OpenUSD only adds the default procedural resolver when HDGP_INCLUDE_DEFAULT_RESOLVER is set;
    // the application owns its Hydra chain, so AIAD appends it explicitly. The whole chain must
    // exist before the stage is set, so every scene index sees PrimsAdded.
    output = HdGpGenerativeProceduralResolvingSceneIndex::New(output);
    indices.stageSceneIndex->SetStage(stage);
    indices.postInstancingNoticeBatchingSceneIndex->Flush();

    Diagnose(output);
    if (!procPath.IsEmpty())
        InspectProcedural(output, procPath);

    int meshes = 0;
    size_t points = 0;
    std::vector<SdfPath> pending{ SdfPath::AbsoluteRootPath() };
    while (!pending.empty())
    {
        const SdfPath path = pending.back();
        pending.pop_back();
        const HdSceneIndexPrim prim = output->GetPrim(path);
        if (prim.primType == TfToken("mesh"))
        {
            ++meshes;
            HdPrimvarsSchema primvars = HdPrimvarsSchema::GetFromParent(prim.dataSource);
            if (HdSampledDataSourceHandle ds = primvars.GetPrimvar(TfToken("points")).GetPrimvarValue())
            {
                const VtValue value = ds->GetValue(0.0f);
                if (value.IsHolding<VtVec3fArray>())
                    points += value.UncheckedGet<VtVec3fArray>().size();
            }
            std::printf("mesh: %s\n", path.GetText());
        }
        for (const SdfPath& child : output->GetChildPrimPaths(path))
            pending.push_back(child);
    }
    std::printf("Hydra meshes: %d, total points: %zu\n", meshes, points);
    const bool ok = brepPrims > 0 && meshes > 0 && points > 0;
    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
