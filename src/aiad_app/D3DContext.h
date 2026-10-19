// Win32 + DirectX 12 presentation for the Dear ImGui UI (adapted from Dear ImGui's
// example_win32_directx12, MIT licence) and an uploadable texture for the Hydra viewport image.
#pragma once

#include <d3d12.h>
#include <dxgi1_5.h>
#include <windows.h>

#include <cstdint>
#include <vector>

namespace aiad::app
{

constexpr int kFramesInFlight = 2;
constexpr int kBackBuffers = 2;

class D3DContext
{
public:
    bool Create(HWND hwnd);
    void Destroy();
    void Resize(UINT width, UINT height);

    // Frame: Begin returns the command list (open, back buffer bound and cleared).
    ID3D12GraphicsCommandList* BeginFrame(const float clear[4]);
    void EndFrame();
    // EndFrame that also reads the back buffer back (RGBA8, top row first): UI regression test.
    std::vector<std::uint8_t> EndFrameWithCapture(int& width, int& height);
    void WaitIdle();

    ID3D12Device* Device() const { return m_device; }
    ID3D12CommandQueue* Queue() const { return m_queue; }
    ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap; }
    void AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
    void FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
    int FrameSlot() const { return int(m_frameIndex % kFramesInFlight); }
    bool Occluded() const { return m_occluded; }
    bool TestOccluded();

private:
    void CreateRenderTargets();
    void ReleaseRenderTargets();

    struct Frame
    {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fence = 0;
    };
    Frame m_frames[kFramesInFlight] = {};
    UINT m_frameIndex = 0;
    ID3D12Device* m_device = nullptr;
    ID3D12DescriptorHeap* m_rtvHeap = nullptr;
    ID3D12DescriptorHeap* m_srvHeap = nullptr;
    std::vector<int> m_srvFree;
    UINT m_srvIncrement = 0;
    ID3D12CommandQueue* m_queue = nullptr;
    ID3D12GraphicsCommandList* m_list = nullptr;
    ID3D12Fence* m_fence = nullptr;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    IDXGISwapChain3* m_swapChain = nullptr;
    HANDLE m_swapWaitable = nullptr;
    bool m_occluded = false;
    ID3D12Resource* m_backBuffers[kBackBuffers] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv[kBackBuffers] = {};
    UINT m_backBufferIndex = 0;
};

// RGBA8 texture updated from CPU memory every frame (one upload buffer per frame in flight).
class UploadTexture
{
public:
    void Create(D3DContext& d3d, int width, int height);
    void Destroy(D3DContext& d3d);
    // Records the copy into the frame's command list; pixels are width*height*4 bytes, top row first.
    void Upload(D3DContext& d3d, ID3D12GraphicsCommandList* list, const std::uint8_t* pixels);
    D3D12_GPU_DESCRIPTOR_HANDLE Srv() const { return m_srvGpu; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }

private:
    int m_width = 0, m_height = 0;
    UINT m_rowPitch = 0;
    ID3D12Resource* m_texture = nullptr;
    ID3D12Resource* m_upload[kFramesInFlight] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvGpu{};
};

}  // namespace aiad::app
