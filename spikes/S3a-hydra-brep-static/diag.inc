// Diagnostics included by main.cpp when built with S3A_DIAG.
#include <pxr/base/tf/type.h>
#include <pxr/usdImaging/usdImaging/adapterRegistry.h>
#include <pxr/imaging/hdGp/generativeProceduralPluginRegistry.h>
#include <pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h>
#include <pxr/imaging/hf/pluginDesc.h>

static void Diagnose(const HdSceneIndexBaseRefPtr& output)
{
    std::printf("adapter for BrepArray: %s\n",
                UsdImagingAdapterRegistry::GetInstance().HasAdapter(TfToken("BrepArray")) ? "yes" : "NO");
    for (const char* name : { "HdUsdBrepArrayAdapter", "HdUsdBrepArrayProceduralPlugin",
                              "HdUsdBrepDisplaySceneIndexPlugin", "HdGpSceneIndexPlugin" })
    {
        const TfType t = TfType::FindByName(name);
        std::printf("TfType %-34s %s\n", name, t.IsUnknown() ? "UNKNOWN" : "registered");
    }
    HfPluginDescVector descs;
    HdGpGenerativeProceduralPluginRegistry::GetInstance().GetPluginDescs(&descs);
    for (const auto& d : descs) std::printf("procedural plugin: %s (%s)\n", d.id.GetText(), d.displayName.c_str());
    std::vector<SdfPath> pending{ SdfPath::AbsoluteRootPath() };
    int shown = 0;
    while (!pending.empty() && shown < 40)
    {
        const SdfPath path = pending.back();
        pending.pop_back();
        const HdSceneIndexPrim prim = output->GetPrim(path);
        std::printf("  %-60s %s\n", path.GetText(), prim.primType.GetText());
        ++shown;
        for (const SdfPath& child : output->GetChildPrimPaths(path))
            pending.push_back(child);
    }
}
