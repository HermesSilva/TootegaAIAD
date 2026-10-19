// Spike S4b: D3D12 <-> CUDA interop on the same adapter (matched by LUID), zero-copy.
// CUDA writes a shared D3D12 buffer, signals a shared D3D12 fence; the D3D12 queue waits on the
// fence, copies the buffer to a readback heap and the CPU verifies the values.
#define NOMINMAX
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define HR(x)                                                                                      \
    do                                                                                             \
    {                                                                                              \
        HRESULT hr_ = (x);                                                                         \
        if (FAILED(hr_))                                                                           \
        {                                                                                          \
            std::printf("[FAIL] %s -> 0x%08lx (line %d)\n", #x, static_cast<unsigned long>(hr_), __LINE__); \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
#define CU(x)                                                                                      \
    do                                                                                             \
    {                                                                                              \
        cudaError_t e_ = (x);                                                                      \
        if (e_ != cudaSuccess)                                                                     \
        {                                                                                          \
            std::printf("[FAIL] %s -> %s (line %d)\n", #x, cudaGetErrorString(e_), __LINE__);     \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

__global__ void FillField(float* field, int n, float scale)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        field[i] = scale * static_cast<float>(i); // stand-in for a simulation result (e.g. stress)
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    constexpr int kCount = 1 << 20; // 1M floats = 4 MB
    constexpr UINT64 kBytes = UINT64(kCount) * sizeof(float);

    // 1) CUDA device and its LUID.
    int cudaDevice = 0;
    CU(cudaSetDevice(cudaDevice));
    cudaDeviceProp prop{};
    CU(cudaGetDeviceProperties(&prop, cudaDevice));
    int fp64Ratio = 0;
    CU(cudaDeviceGetAttribute(&fp64Ratio, cudaDevAttrSingleToDoublePrecisionPerfRatio, cudaDevice));
    std::printf("CUDA device: %s, cc %d.%d, FP32:FP64 = %d:1 -> %s\n", prop.name, prop.major, prop.minor,
                fp64Ratio, fp64Ratio > 4 ? "FP64-limited, AIAD would pick fp32" : "full FP64");

    // 2) DXGI adapter with the same LUID.
    ComPtr<IDXGIFactory6> factory;
    HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    bool matched = false;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (std::memcmp(&desc.AdapterLuid, prop.luid, sizeof(LUID)) == 0)
        {
            std::printf("DXGI adapter %u matches the CUDA LUID: %ls\n", i, desc.Description);
            matched = true;
            break;
        }
    }
    if (!matched)
    {
        std::printf("[FAIL] no DXGI adapter with the CUDA device LUID\n");
        return 1;
    }

    ComPtr<ID3D12Device> device;
    HR(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)));
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opt5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opt5, sizeof(opt5));
    std::printf("D3D12 device created; raytracing tier %d\n", int(opt5.RaytracingTier));

    // 3) Shared default-heap buffer (written by CUDA, read by D3D12).
    D3D12_HEAP_PROPERTIES heapDefault{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC bufDesc{};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = kBytes;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> shared;
    HR(device->CreateCommittedResource(&heapDefault, D3D12_HEAP_FLAG_SHARED, &bufDesc,
                                       D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&shared)));
    HANDLE sharedHandle = nullptr;
    HR(device->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle));

    // 4) Import into CUDA.
    cudaExternalMemoryHandleDesc memDesc{};
    memDesc.type = cudaExternalMemoryHandleTypeD3D12Resource;
    memDesc.handle.win32.handle = sharedHandle;
    memDesc.size = device->GetResourceAllocationInfo(0, 1, &bufDesc).SizeInBytes;
    memDesc.flags = cudaExternalMemoryDedicated;
    cudaExternalMemory_t extMem = nullptr;
    CU(cudaImportExternalMemory(&extMem, &memDesc));
    cudaExternalMemoryBufferDesc bd{};
    bd.offset = 0;
    bd.size = kBytes;
    void* devPtr = nullptr;
    CU(cudaExternalMemoryGetMappedBuffer(&devPtr, extMem, &bd));

    // 5) Shared fence.
    ComPtr<ID3D12Fence> fence;
    HR(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)));
    HANDLE fenceHandle = nullptr;
    HR(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle));
    cudaExternalSemaphoreHandleDesc semDesc{};
    semDesc.type = cudaExternalSemaphoreHandleTypeD3D12Fence;
    semDesc.handle.win32.handle = fenceHandle;
    cudaExternalSemaphore_t extSem = nullptr;
    CU(cudaImportExternalSemaphore(&extSem, &semDesc));

    // 6) CUDA writes the field and signals the fence (value 1).
    cudaStream_t stream = nullptr;
    CU(cudaStreamCreate(&stream));
    const float scale = 0.5f;
    FillField<<<(kCount + 255) / 256, 256, 0, stream>>>(static_cast<float*>(devPtr), kCount, scale);
    CU(cudaGetLastError());
    cudaExternalSemaphoreSignalParams sig{};
    sig.params.fence.value = 1;
    CU(cudaSignalExternalSemaphoresAsync(&extSem, &sig, 1, stream));

    // 7) D3D12 waits on the fence, copies to a readback buffer.
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT };
    ComPtr<ID3D12CommandQueue> queue;
    HR(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> alloc;
    HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list;
    HR(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)));
    D3D12_HEAP_PROPERTIES heapReadback{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rbDesc = bufDesc;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> readback;
    HR(device->CreateCommittedResource(&heapReadback, D3D12_HEAP_FLAG_NONE, &rbDesc,
                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    list->CopyBufferRegion(readback.Get(), 0, shared.Get(), 0, kBytes);
    HR(list->Close());

    HR(queue->Wait(fence.Get(), 1)); // GPU-side wait for CUDA
    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    HR(queue->Signal(fence.Get(), 2));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HR(fence->SetEventOnCompletion(2, evt));
    WaitForSingleObject(evt, 10000);
    CloseHandle(evt);

    // 8) Verify.
    float* mapped = nullptr;
    D3D12_RANGE range{ 0, SIZE_T(kBytes) };
    HR(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)));
    int bad = 0;
    for (int i = 0; i < kCount; ++i)
        if (mapped[i] != scale * static_cast<float>(i))
            ++bad;
    std::printf("sample values: [0]=%g [1]=%g [last]=%g\n", mapped[0], mapped[1], mapped[kCount - 1]);
    readback->Unmap(0, nullptr);

    CU(cudaStreamDestroy(stream));
    CU(cudaDestroyExternalSemaphore(extSem));
    CU(cudaDestroyExternalMemory(extMem));
    CloseHandle(fenceHandle);
    CloseHandle(sharedHandle);

    std::printf("mismatches: %d of %d\n", bad, kCount);
    std::printf("%s\n", bad == 0 ? "ALL PASS" : "FAILURES");
    return bad == 0 ? 0 : 1;
}
