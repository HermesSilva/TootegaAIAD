// Spike S3b: render a USD BRep BrepArray with Hydra + HdAurora (DirectX 12 / DXR path tracing),
// everything statically linked (OpenUSD, USD BRep, hdUsdBrep, Aurora, HdAurora). No Hgi, no OpenGL:
// the colour AOV is read back through HdRenderBuffer::Map() and written to a PNG.
#include <pxr/pxr.h>
#include <pxr/base/gf/half.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/range3d.h>
#include <pxr/base/plug/registry.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/sceneIndexPluginRegistry.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/xformable.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

// HdAurora headers expect the context of its pch.h (pxr names, Aurora API).
#include <Aurora/Aurora.h>
#include "HdAuroraRenderBuffer.h"

namespace
{
// Minimal task: sync and execute one render pass (hdx is not available without a GPU backend).
class RenderTask final : public HdTask
{
public:
    RenderTask(const SdfPath& id, HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state) :
        HdTask(id), _pass(std::move(pass)), _state(std::move(state))
    {
    }
    void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirtyBits) override
    {
        _pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext*, HdRenderIndex* index) override
    {
        _state->Prepare(index->GetResourceRegistry());
    }
    void Execute(HdTaskContext*) override { _pass->Execute(_state, GetRenderTags()); }
    const TfTokenVector& GetRenderTags() const override
    {
        static const TfTokenVector tags{ HdRenderTagTokens->geometry };
        return tags;
    }
    bool IsConverged() const { return _pass->IsConverged(); }

private:
    HdRenderPassSharedPtr _pass;
    HdRenderPassStateSharedPtr _state;
};
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 5)
    {
        std::printf("usage: s3b_aurora <plugins-dir> <hdAurora-resources> <asset.usda> <out.png>\n");
        return 2;
    }
    const std::string pluginsDir = argv[1];
    const std::string auroraResources = argv[2];
    const std::string asset = argv[3];
    const std::string outPng = argv[4];
    const int width = 800, height = 600;

    PlugRegistry::GetInstance().RegisterPlugins(std::vector<std::string>{
        pluginsDir + "/hdUsdBrep", pluginsDir + "/omniSolid/resources", auroraResources });

    UsdStageRefPtr stage = UsdStage::Open(asset);
    if (!stage)
    {
        std::printf("[FAIL] cannot open %s\n", asset.c_str());
        return 1;
    }

    // Camera framing the whole stage, authored in the session layer.
    UsdGeomBBoxCache bboxCache(UsdTimeCode::Default(), { UsdGeomTokens->default_ });
    const GfRange3d box = bboxCache.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
    const GfVec3d center = box.GetMidpoint();
    const double radius = std::max(box.GetSize().GetLength() * 0.5, 1e-3);
    const GfVec3d eye = center + GfVec3d(1.0, -1.6, 1.0).GetNormalized() * radius * 3.2;
    stage->SetEditTarget(stage->GetSessionLayer());
    UsdGeomCamera camera = UsdGeomCamera::Define(stage, SdfPath("/AiadCamera"));
    GfMatrix4d view;
    view.SetLookAt(eye, center, GfVec3d(0, 0, 1));
    camera.MakeMatrixXform().Set(view.GetInverse());
    camera.GetFocalLengthAttr().Set(35.0f);
    camera.GetHorizontalApertureAttr().Set(36.0f);
    camera.GetVerticalApertureAttr().Set(36.0f * height / width);
    camera.GetClippingRangeAttr().Set(GfVec2f(float(radius * 0.01), float(radius * 100.0)));
    std::printf("scene bounds: center (%.3f %.3f %.3f), radius %.3f\n", center[0], center[1], center[2], radius);

    // Renderer: HdAurora, linked into this executable.
    HdRendererPluginHandle plugin =
        HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(TfToken("HdAuroraRendererPlugin"));
    if (!plugin)
    {
        std::printf("[FAIL] HdAurora renderer plugin not found\n");
        return 1;
    }
    HdRenderDelegate* delegate = plugin->CreateRenderDelegate();
    HdRenderIndex* index = HdRenderIndex::New(delegate, HdDriverVector{});

    // Scene: stage scene index -> plugins -> procedural resolver (hdUsdBrep tessellation).
    UsdImagingCreateSceneIndicesInfo info;
    UsdImagingSceneIndices indices = UsdImagingCreateSceneIndices(info);
    HdSceneIndexBaseRefPtr scene =
        HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer("Aurora", indices.finalSceneIndex);
    scene = HdGpGenerativeProceduralResolvingSceneIndex::New(scene);
    index->InsertSceneIndex(scene, SdfPath::AbsoluteRootPath());
    indices.stageSceneIndex->SetStage(stage);
    indices.postInstancingNoticeBatchingSceneIndex->Flush();

    // Colour AOV owned by us.
    auto* colour = static_cast<HdRenderBuffer*>(
        delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/AiadAov/color")));
    colour->Allocate(GfVec3i(width, height, 1), HdFormatUNorm8Vec4, false);

    HdRenderPassSharedPtr pass = delegate->CreateRenderPass(
        index, HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
    HdRenderPassStateSharedPtr state = delegate->CreateRenderPassState();
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = colour;
    binding.clearValue = VtValue(GfVec4f(0.12f, 0.12f, 0.14f, 1.0f));
    state->SetAovBindings({ binding });
    state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));

    auto task = std::make_shared<RenderTask>(SdfPath("/AiadRenderTask"), pass, state);
    HdTaskSharedPtrVector tasks{ task };
    HdEngine engine;

    const auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    for (; frames < 512; ++frames)
    {
        if (!state->GetCamera())
        {
            const auto* cam = static_cast<const HdCamera*>(
                index->GetSprim(HdPrimTypeTokens->camera, SdfPath("/AiadCamera")));
            state->SetCamera(cam);
        }
        engine.Execute(index, &tasks);
        if (frames > 4 && task->IsConverged())
            break;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("frames: %d, converged: %s, %.2f s\n", frames + 1, task->IsConverged() ? "yes" : "no", seconds);

    // Read back and write PNG.
    colour->Resolve();
    const auto* pixels = static_cast<const unsigned char*>(colour->Map());
    // Aurora aligns rows; HdAuroraRenderBuffer exposes the real row pitch.
    auto* auroraColour = dynamic_cast<HdAuroraRenderBuffer*>(colour);
    const size_t rowPitch = auroraColour && auroraColour->GetStride() ? auroraColour->GetStride() : size_t(width) * 4;
    std::printf("row pitch: %zu bytes (width x 4 = %d)\n", rowPitch, width * 4);
    // Bytes per pixel from the real pitch: 4 = 8-bit RGBA, 8 = half-float RGBA (linear).
    const size_t bytesPerPixel = rowPitch / size_t(width) >= 8 ? 8 : 4;
    std::printf("pixel format: %s\n", bytesPerPixel == 8 ? "half-float RGBA" : "8-bit RGBA");
    auto toSrgb8 = [](float linear) {
        const float c = std::clamp(linear, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
        return static_cast<unsigned char>(std::lround(s * 255.0f));
    };
    int litPixels = 0;
    std::vector<unsigned char> rgba(size_t(width) * height * 4);
    if (pixels)
    {
        // First pixel = background; count pixels that differ from it (the rendered object).
        float background[3] = { 0, 0, 0 };
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                const unsigned char* p = pixels + size_t(y) * rowPitch + size_t(x) * bytesPerPixel;
                float c[3];
                for (int k = 0; k < 3; ++k)
                    c[k] = bytesPerPixel == 8 ? float(reinterpret_cast<const GfHalf*>(p)[k]) : p[k] / 255.0f;
                if (x == 0 && y == 0)
                    for (int k = 0; k < 3; ++k)
                        background[k] = c[k];
                unsigned char* q = rgba.data() + (size_t(height - 1 - y) * width + x) * 4;
                for (int k = 0; k < 3; ++k)
                    q[k] = toSrgb8(c[k]);
                q[3] = 255;
                if (std::abs(c[0] - background[0]) + std::abs(c[1] - background[1]) + std::abs(c[2] - background[2]) > 0.05f)
                    ++litPixels;
            }
    }
    const int nonBackground = litPixels;
    colour->Unmap();
    const bool written = stbi_write_png(outPng.c_str(), width, height, 4, rgba.data(), width * 4) != 0;
    std::printf("image: %s (%s), non-background pixels: %d\n", outPng.c_str(), written ? "written" : "NOT written",
                nonBackground);

    tasks.clear();
    task.reset();
    pass.reset();
    state.reset();
    delegate->DestroyBprim(colour);
    delete index;
    plugin->DeleteRenderDelegate(delegate);

    const bool ok = pixels && written && nonBackground > 500;
    std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
