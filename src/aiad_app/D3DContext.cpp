#include "D3DContext.h"

#include <cstdio>
#include <cstring>

namespace aiad::app
{

namespace
{

constexpr int kSrvHeapSize = 256;  // fonts, viewport, chat image thumbnails

template <typename T>
void SafeRelease(T*& p)
{
    if (p)
    {
        p->Release();
        p = nullptr;
    }
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

}  // namespace

bool D3DContext::Create(HWND hwnd)
{
    if (D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)) != S_OK)
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv = {};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = kBackBuffers;
    if (m_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&m_rtvHeap)) != S_OK)
        return false;
    const UINT rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (int i = 0; i < kBackBuffers; ++i, handle.ptr += rtvSize)
        m_rtv[i] = handle;

    D3D12_DESCRIPTOR_HEAP_DESC srv = {};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = kSrvHeapSize;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (m_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&m_srvHeap)) != S_OK)
        return false;
    m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int i = kSrvHeapSize - 1; i >= 0; --i)
        m_srvFree.push_back(i);

    D3D12_COMMAND_QUEUE_DESC q = {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue)) != S_OK)
        return false;
    for (Frame& f : m_frames)
        if (m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)) != S_OK)
            return false;
    if (m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator, nullptr, IID_PPV_ARGS(&m_list)) != S_OK ||
        m_list->Close() != S_OK)
        return false;
    if (m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)) != S_OK)
        return false;
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.BufferCount = kBackBuffers;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    IDXGIFactory5* factory = nullptr;
    IDXGISwapChain1* swap1 = nullptr;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factory)) != S_OK ||
        factory->CreateSwapChainForHwnd(m_queue, hwnd, &sd, nullptr, nullptr, &swap1) != S_OK ||
        swap1->QueryInterface(IID_PPV_ARGS(&m_swapChain)) != S_OK)
        return false;
    swap1->Release();
    factory->Release();
    m_swapChain->SetMaximumFrameLatency(kBackBuffers);
    m_swapWaitable = m_swapChain->GetFrameLatencyWaitableObject();
    CreateRenderTargets();
    return true;
}

void D3DContext::Destroy()
{
    if (m_queue && m_fence)
        WaitIdle();
    ReleaseRenderTargets();
    SafeRelease(m_swapChain);
    if (m_swapWaitable)
        CloseHandle(m_swapWaitable);
    for (Frame& f : m_frames)
        SafeRelease(f.allocator);
    SafeRelease(m_list);
    SafeRelease(m_queue);
    SafeRelease(m_rtvHeap);
    SafeRelease(m_srvHeap);
    SafeRelease(m_fence);
    if (m_fenceEvent)
        CloseHandle(m_fenceEvent);
    SafeRelease(m_device);
}

void D3DContext::CreateRenderTargets()
{
    for (int i = 0; i < kBackBuffers; ++i)
    {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
        m_device->CreateRenderTargetView(m_backBuffers[i], nullptr, m_rtv[i]);
    }
}

void D3DContext::ReleaseRenderTargets()
{
    for (auto*& b : m_backBuffers)
        SafeRelease(b);
}

void D3DContext::Resize(UINT width, UINT height)
{
    if (!m_swapChain)
        return;
    WaitIdle();
    ReleaseRenderTargets();
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    m_swapChain->GetDesc1(&desc);
    m_swapChain->ResizeBuffers(0, width, height, desc.Format, desc.Flags);
    CreateRenderTargets();
}

void D3DContext::WaitIdle()
{
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

bool D3DContext::TestOccluded()
{
    if (m_occluded && m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        return true;
    m_occluded = false;
    return false;
}

ID3D12GraphicsCommandList* D3DContext::BeginFrame(const float clear[4])
{
    Frame& frame = m_frames[m_frameIndex % kFramesInFlight];
    if (m_fence->GetCompletedValue() < frame.fence)
    {
        m_fence->SetEventOnCompletion(frame.fence, m_fenceEvent);
        HANDLE waits[] = {m_swapWaitable, m_fenceEvent};
        if (WaitForMultipleObjects(2, waits, TRUE, 10000) == WAIT_TIMEOUT)
            std::fprintf(stderr, "BeginFrame: GPU frame not done after 10 s (fence %llu of %llu, device removed reason 0x%08lx)\n",
                         m_fence->GetCompletedValue(), frame.fence, unsigned long(m_device->GetDeviceRemovedReason()));
    }
    else if (WaitForSingleObject(m_swapWaitable, 10000) == WAIT_TIMEOUT)
        std::fprintf(stderr, "BeginFrame: swap chain not ready after 10 s (device removed reason 0x%08lx)\n", unsigned long(m_device->GetDeviceRemovedReason()));

    m_backBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    frame.allocator->Reset();
    m_list->Reset(frame.allocator, nullptr);
    const D3D12_RESOURCE_BARRIER toTarget =
        Transition(m_backBuffers[m_backBufferIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_list->ResourceBarrier(1, &toTarget);
    m_list->ClearRenderTargetView(m_rtv[m_backBufferIndex], clear, 0, nullptr);
    m_list->OMSetRenderTargets(1, &m_rtv[m_backBufferIndex], FALSE, nullptr);
    m_list->SetDescriptorHeaps(1, &m_srvHeap);
    return m_list;
}

void D3DContext::EndFrame()
{
    const D3D12_RESOURCE_BARRIER toPresent =
        Transition(m_backBuffers[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_list->ResourceBarrier(1, &toPresent);
    m_list->Close();
    ID3D12CommandList* lists[] = {m_list};
    m_queue->ExecuteCommandLists(1, lists);
    m_queue->Signal(m_fence, ++m_fenceValue);
    m_frames[m_frameIndex % kFramesInFlight].fence = m_fenceValue;
    m_occluded = m_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    ++m_frameIndex;
}

std::vector<uint8_t> D3DContext::EndFrameWithCapture(int& width, int& height)
{
    ID3D12Resource* back = m_backBuffers[m_backBufferIndex];
    const D3D12_RESOURCE_DESC desc = back->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* readback = nullptr;
    m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    const D3D12_RESOURCE_BARRIER toCopy = Transition(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_list->ResourceBarrier(1, &toCopy);
    D3D12_TEXTURE_COPY_LOCATION source{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION target{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    target.PlacedFootprint = footprint;
    m_list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
    const D3D12_RESOURCE_BARRIER back2 = Transition(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_list->ResourceBarrier(1, &back2);
    EndFrame();
    WaitIdle();
    width = int(desc.Width);
    height = int(desc.Height);
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    uint8_t* mapped = nullptr;
    readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    for (int y = 0; y < height; ++y)
        std::memcpy(&pixels[size_t(y) * width * 4], mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, size_t(width) * 4);
    readback->Unmap(0, nullptr);
    readback->Release();
    for (size_t i = 3; i < pixels.size(); i += 4)
        pixels[i] = 255;
    return pixels;
}

void D3DContext::AllocSrv(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const int index = m_srvFree.back();
    m_srvFree.pop_back();
    cpu->ptr = m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * m_srvIncrement;
    gpu->ptr = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * m_srvIncrement;
}

void D3DContext::FreeSrv(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    m_srvFree.push_back(int((cpu.ptr - m_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / m_srvIncrement));
}

void UploadTexture::Create(D3DContext& d3d, int width, int height)
{
    m_width = width;
    m_height = height;
    m_rowPitch = (UINT(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = UINT64(width);
    desc.Height = UINT(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    d3d.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                          nullptr, IID_PPV_ARGS(&m_texture));

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64(m_rowPitch) * UINT(height);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (auto*& u : m_upload)
        d3d.Device()->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                              IID_PPV_ARGS(&u));

    d3d.AllocSrv(&m_srvCpu, &m_srvGpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d3d.Device()->CreateShaderResourceView(m_texture, &view, m_srvCpu);
}

void UploadTexture::Destroy(D3DContext& d3d)
{
    if (!m_texture)
        return;
    d3d.FreeSrv(m_srvCpu, m_srvGpu);
    SafeRelease(m_texture);
    for (auto*& u : m_upload)
        SafeRelease(u);
    m_width = m_height = 0;
}

void UploadTexture::Upload(D3DContext& d3d, ID3D12GraphicsCommandList* list, const std::uint8_t* pixels)
{
    ID3D12Resource* upload = m_upload[d3d.FrameSlot()];
    void* mapped = nullptr;
    const D3D12_RANGE none = {0, 0};
    upload->Map(0, &none, &mapped);
    for (int y = 0; y < m_height; ++y)
        std::memcpy(static_cast<std::uint8_t*>(mapped) + size_t(y) * m_rowPitch, pixels + size_t(y) * m_width * 4, size_t(m_width) * 4);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = UINT(m_width);
    src.PlacedFootprint.Footprint.Height = UINT(m_height);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = m_rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    const D3D12_RESOURCE_BARRIER toCopy =
        Transition(m_texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &toCopy);
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    const D3D12_RESOURCE_BARRIER toRead =
        Transition(m_texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &toRead);
}

}  // namespace aiad::app
