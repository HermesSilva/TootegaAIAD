// Hydra viewport rendered by HdAurora (DirectX 12 / DXR path tracing), statically linked.
//
// Aurora owns its own D3D12 device: every frame accumulates one sample into a colour AOV that the
// caller reads back as 8-bit sRGB RGBA (for upload into the UI texture).
#pragma once

#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usd/stage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aiad::render
{

// Registers plugInfo folders (hdUsdBrep, omniSolid, hdAurora) before any stage is opened.
void RegisterPlugins(const std::vector<std::string>& pluginDirs);

struct ViewportImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;   // sRGB, row-major, top row first
};

class HydraViewport
{
public:
    HydraViewport();
    ~HydraViewport();
    HydraViewport(const HydraViewport&) = delete;
    HydraViewport& operator=(const HydraViewport&) = delete;

    void SetStage(const pxr::UsdStageRefPtr& stage);
    // cameraToWorld: camera looks down its -Z with +Y up (USD convention).
    void SetCamera(const pxr::GfMatrix4d& cameraToWorld, double verticalFovDeg, double nearClip, double farClip);
    void Resize(int width, int height);

    // Syncs pending stage edits and accumulates one more sample.
    void Render();
    bool IsConverged() const;
    const ViewportImage& ReadImage();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace aiad::render
