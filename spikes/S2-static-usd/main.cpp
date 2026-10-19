// Spike S2b: smoke test of the static OpenUSD 25.11 build (no DLLs, plugins linked in).
// Checks: stage authoring, schema registry, UsdImaging adapter registry, usdMtlx file format.
#include <pxr/pxr.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/type.h>
#include <pxr/usd/sdf/fileFormat.h>
#include <pxr/usd/usd/schemaRegistry.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usdImaging/usdImaging/adapterRegistry.h>

#include <cstdio>

PXR_NAMESPACE_USING_DIRECTIVE

static int g_failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const PlugPluginPtrVector plugins = PlugRegistry::GetInstance().GetAllPlugins();
    std::printf("registered plugins: %zu\n", plugins.size());
    Check(plugins.size() > 20, "plugInfo resources found and registered");

    UsdStageRefPtr stage = UsdStage::CreateInMemory();
    UsdGeomSphere sphere = UsdGeomSphere::Define(stage, SdfPath("/World/Ball"));
    sphere.GetRadiusAttr().Set(2.5);
    double radius = 0.0;
    sphere.GetRadiusAttr().Get(&radius);
    Check(radius == 2.5, "stage authoring (UsdGeomSphere radius round-trip)");

    const TfType meshType = UsdSchemaRegistry::GetTypeFromSchemaTypeName(TfToken("Mesh"));
    Check(!meshType.IsUnknown(), "schema registry knows 'Mesh' (usdGeom plugin)");

    Check(UsdImagingAdapterRegistry::GetInstance().HasAdapter(TfToken("Mesh")),
          "UsdImaging adapter registry has an adapter for 'Mesh'");

    Check(static_cast<bool>(SdfFileFormat::FindByExtension("mtlx")),
          "usdMtlx file format registered for .mtlx");

    std::string text;
    stage->GetRootLayer()->ExportToString(&text);
    Check(text.find("Ball") != std::string::npos, "layer export");

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
