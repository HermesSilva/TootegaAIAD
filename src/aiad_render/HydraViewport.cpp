#include "HydraViewport.h"

#include <pxr/base/gf/half.h>
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
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdGp/generativeProceduralResolvingSceneIndex.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace aiad::render
{

namespace
{

const SdfPath kCameraPath("/AiadViewportCamera");

// Minimal task: sync and execute one render pass (hdx needs a GPU Hgi backend, which we do not use).
class RenderTask final : public HdTask
{
public:
    RenderTask(const SdfPath& id, HdRenderPassSharedPtr pass, HdRenderPassStateSharedPtr state)
        : HdTask(id), m_pass(std::move(pass)), m_state(std::move(state))
    {
    }
    void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirtyBits) override
    {
        m_pass->Sync();
        *dirtyBits = HdChangeTracker::Clean;
    }
    void Prepare(HdTaskContext*, HdRenderIndex* index) override { m_state->Prepare(index->GetResourceRegistry()); }
    void Execute(HdTaskContext*) override { m_pass->Execute(m_state, GetRenderTags()); }
    const TfTokenVector& GetRenderTags() const override
    {
        static const TfTokenVector tags{HdRenderTagTokens->geometry};
        return tags;
    }
    bool IsConverged() const { return m_pass->IsConverged(); }

private:
    HdRenderPassSharedPtr m_pass;
    HdRenderPassStateSharedPtr m_state;
};

// Linear half-float -> sRGB 8-bit, indexed by the 16 raw bits of the half.
const std::array<std::uint8_t, 65536>& HalfToSrgb()
{
    static const std::array<std::uint8_t, 65536> table = [] {
        std::array<std::uint8_t, 65536> t{};
        for (std::uint32_t bits = 0; bits < 65536; ++bits)
        {
            GfHalf h;
            h.setBits(static_cast<unsigned short>(bits));
            float c = std::isfinite(float(h)) ? std::clamp(float(h), 0.0f, 1.0f) : 0.0f;
            c = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            t[bits] = static_cast<std::uint8_t>(std::lround(c * 255.0f));
        }
        return t;
    }();
    return table;
}

}  // namespace

void RegisterPlugins(const std::vector<std::string>& pluginDirs)
{
    PlugRegistry::GetInstance().RegisterPlugins(pluginDirs);
}

struct HydraViewport::Impl
{
    HdRendererPluginHandle plugin;
    HdRenderDelegate* delegate = nullptr;
    HdRenderIndex* index = nullptr;
    UsdImagingSceneIndices sceneIndices;
    HdRenderBuffer* colour = nullptr;
    HdRenderPassSharedPtr pass;
    HdRenderPassStateSharedPtr state;
    std::shared_ptr<RenderTask> task;
    HdEngine engine;
    UsdStageRefPtr stage;
    int width = 0, height = 0;
    ViewportImage image;
};

HydraViewport::HydraViewport() : m_impl(std::make_unique<Impl>())
{
    Impl& m = *m_impl;
    m.plugin = HdRendererPluginRegistry::GetInstance().GetOrCreateRendererPlugin(TfToken("HdAuroraRendererPlugin"));
    if (!m.plugin)
        throw std::runtime_error("HdAurora renderer plugin not found (is hdAurora linked with /WHOLEARCHIVE?)");
    m.delegate = m.plugin->CreateRenderDelegate();
    // CAD-style background: blue-grey vertical gradient (top, bottom); the dome light only lights the
    // scene, it is not shown behind it.
    m.delegate->SetRenderSetting(TfToken("aurora:use_environment_image_as_background"), VtValue(false));
    // Animated scenes restart the path tracer every frame: denoise (NRD) instead of showing raw samples.
    m.delegate->SetRenderSetting(TfToken("aurora:is_denoising_enabled"), VtValue(true));
    m.delegate->SetRenderSetting(TfToken("aurora:background_colors"),
                                 VtValue(VtVec3fArray{GfVec3f(0.13f, 0.16f, 0.23f), GfVec3f(0.52f, 0.56f, 0.64f)}));
    m.index = HdRenderIndex::New(m.delegate, HdDriverVector{});

    // Stage scene index -> renderer plugins -> procedural resolver (hdUsdBrep tessellation).
    m.sceneIndices = UsdImagingCreateSceneIndices(UsdImagingCreateSceneIndicesInfo{});
    HdSceneIndexBaseRefPtr scene =
        HdSceneIndexPluginRegistry::GetInstance().AppendSceneIndicesForRenderer("Aurora", m.sceneIndices.finalSceneIndex);
    scene = HdGpGenerativeProceduralResolvingSceneIndex::New(scene);
    m.index->InsertSceneIndex(scene, SdfPath::AbsoluteRootPath());

    m.pass = m.delegate->CreateRenderPass(m.index, HdRprimCollection(HdTokens->geometry, HdReprSelector(HdReprTokens->refined)));
    m.state = m.delegate->CreateRenderPassState();
    m.task = std::make_shared<RenderTask>(SdfPath("/AiadViewportTask"), m.pass, m.state);
    Resize(640, 480);
}

HydraViewport::~HydraViewport()
{
    Impl& m = *m_impl;
    m.task.reset();
    m.pass.reset();
    m.state.reset();
    if (m.colour)
        m.delegate->DestroyBprim(m.colour);
    delete m.index;
    m.plugin->DeleteRenderDelegate(m.delegate);
}

void HydraViewport::SetStage(const UsdStageRefPtr& stage)
{
    Impl& m = *m_impl;
    m.stage = stage;
    m.sceneIndices.stageSceneIndex->SetStage(stage);
    m.sceneIndices.postInstancingNoticeBatchingSceneIndex->Flush();
}

void HydraViewport::SetCamera(const GfMatrix4d& cameraToWorld, double verticalFovDeg, double nearClip, double farClip)
{
    Impl& m = *m_impl;
    if (!m.stage)
        return;
    // The camera lives in the session layer: it is a view setting, not part of the document.
    const UsdEditTarget previous = m.stage->GetEditTarget();
    m.stage->SetEditTarget(m.stage->GetSessionLayer());
    UsdGeomCamera camera = UsdGeomCamera::Define(m.stage, kCameraPath);
    const float verticalAperture = 24.0f;
    const float focal = float(verticalAperture / 2.0 / std::tan(verticalFovDeg * 3.14159265358979 / 360.0));
    camera.MakeMatrixXform().Set(cameraToWorld);
    camera.GetFocalLengthAttr().Set(focal);
    camera.GetVerticalApertureAttr().Set(verticalAperture);
    camera.GetHorizontalApertureAttr().Set(verticalAperture * float(m.width) / float(std::max(m.height, 1)));
    camera.GetClippingRangeAttr().Set(GfVec2f(float(nearClip), float(farClip)));
    m.stage->SetEditTarget(previous);
}

void HydraViewport::Resize(int width, int height)
{
    Impl& m = *m_impl;
    width = std::max(width, 16);
    height = std::max(height, 16);
    if (width == m.width && height == m.height)
        return;
    m.width = width;
    m.height = height;
    if (m.colour)
        m.delegate->DestroyBprim(m.colour);
    m.colour = static_cast<HdRenderBuffer*>(m.delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/AiadViewportAov/color")));
    m.colour->Allocate(GfVec3i(width, height, 1), HdFormatUNorm8Vec4, false);
    HdRenderPassAovBinding binding;
    binding.aovName = HdAovTokens->color;
    binding.renderBuffer = m.colour;
    binding.clearValue = VtValue(GfVec4f(0.16f, 0.17f, 0.19f, 1.0f));
    m.state->SetAovBindings({binding});
    m.state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
}

void HydraViewport::Render()
{
    Impl& m = *m_impl;
    if (!m.stage)
        return;
    // Push the stage edits of this frame (poses, camera) into the scene index.
    m.sceneIndices.stageSceneIndex->ApplyPendingUpdates();
    m.sceneIndices.postInstancingNoticeBatchingSceneIndex->Flush();
    if (!m.state->GetCamera())
        m.state->SetCamera(static_cast<const HdCamera*>(m.index->GetSprim(HdPrimTypeTokens->camera, kCameraPath)));
    HdTaskSharedPtrVector tasks{m.task};
    m.engine.Execute(m.index, &tasks);
}

bool HydraViewport::IsConverged() const
{
    return m_impl->task->IsConverged();
}

const ViewportImage& HydraViewport::ReadImage()
{
    Impl& m = *m_impl;
    ViewportImage& image = m.image;

    // The render pass reallocates the buffer to the size it renders at, which can lag behind
    // Resize() for a frame or more (e.g. while a window is dragged): always read with the buffer's own
    // dimensions and row pitch, never with the requested size. The caller skips mismatched frames.
    m.colour->Resolve();
    const int width = int(m.colour->GetWidth()), height = int(m.colour->GetHeight());
    // Map() returns tightly packed rows (Aurora's data(stride, removePadding = true)), bottom row
    // first: the pitch is width * pixel size. GetStride() may hold the padded GPU row pitch instead.
    const HdFormat format = m.colour->GetFormat();
    const size_t bytesPerPixel = HdDataSizeOfFormat(format);
    const size_t pitch = size_t(width) * bytesPerPixel;
    const auto* pixels = static_cast<const std::uint8_t*>(m.colour->Map());
    if (!pixels || width <= 1 || height <= 1 || (format != HdFormatFloat16Vec4 && format != HdFormatUNorm8Vec4))
    {
        m.colour->Unmap();
        return image;
    }
    const bool half = format == HdFormatFloat16Vec4;
    image.width = width;
    image.height = height;
    image.rgba.resize(size_t(width) * height * 4);
    const auto& lut = HalfToSrgb();
    for (int y = 0; y < height; ++y)
    {
        const std::uint8_t* src = pixels + size_t(y) * pitch;
        std::uint8_t* dst = image.rgba.data() + size_t(height - 1 - y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            if (half)
            {
                const auto* h = reinterpret_cast<const std::uint16_t*>(src + size_t(x) * 8);
                dst[x * 4 + 0] = lut[h[0]];
                dst[x * 4 + 1] = lut[h[1]];
                dst[x * 4 + 2] = lut[h[2]];
            }
            else
            {
                dst[x * 4 + 0] = src[x * 4 + 0];
                dst[x * 4 + 1] = src[x * 4 + 1];
                dst[x * 4 + 2] = src[x * 4 + 2];
            }
            dst[x * 4 + 3] = 255;
        }
    }
    m.colour->Unmap();
    return image;
}

}  // namespace aiad::render
