#include "VulkanD3D12Interop.h"

#include <iostream>
#include <sstream>

// Vulkan headers (use Diligent's wrapper to ensure correct include paths)
#define VK_USE_PLATFORM_WIN32_KHR
#include "VulkanUtilities/VulkanHeaders.h"

// Diligent Vulkan 接口
#include "DeviceContextVk.h"
#include "RenderDeviceVk.h"
#include "TextureVk.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

// 日志宏：输出到 DebugView
#define VK_LOG(msg)                                                                                                    \
    do {                                                                                                               \
        std::ostringstream oss;                                                                                        \
        oss << msg;                                                                                                    \
        OutputDebugStringA(oss.str().c_str());                                                                         \
        OutputDebugStringA("\n");                                                                                      \
    } while (0)

#define VK_ERR(msg)                                                                                                    \
    do {                                                                                                               \
        std::ostringstream oss;                                                                                        \
        oss << msg;                                                                                                    \
        OutputDebugStringA(oss.str().c_str());                                                                         \
        OutputDebugStringA("\n");                                                                                      \
    } while (0)

// 类型转换宏（将 opaque void* 转为 Vulkan 类型）
#define VK_DEVICE (reinterpret_cast<VkDevice>(vkNativeDevice_))
#define VK_PHYS_DEVICE (reinterpret_cast<VkPhysicalDevice>(vkPhysicalDevice_))
#define VK_IMAGE (reinterpret_cast<VkImage>(vkSharedImage_))
#define VK_MEMORY (reinterpret_cast<VkDeviceMemory>(vkSharedMemory_))

using namespace Diligent;

namespace ParticleSaturn::Render {

VulkanD3D12Interop::~VulkanD3D12Interop() {
    Shutdown();
}

bool VulkanD3D12Interop::Init(HWND hwnd, IRenderDevice* vkDevice, IDeviceContext* vkContext, uint32_t width,
                              uint32_t height) {
    OutputDebugStringA("[VkD3D12] >>> Init() ENTRY <<<\n");

    if (hwnd == nullptr || vkDevice == nullptr || vkContext == nullptr) {
        OutputDebugStringA("[VkD3D12] Invalid parameters!\n");
        return false;
    }

    hwnd_      = hwnd;
    vkDevice_  = vkDevice;
    vkContext_ = vkContext;
    width_     = width;
    height_    = height;

    VK_LOG("[VkD3D12] Init starting: hwnd=" << hwnd << ", device=" << vkDevice << ", context=" << vkContext << ", "
                                            << width << "x" << height);

    // 1. 创建 D3D12 设备
    VK_LOG("[VkD3D12] Step 1: Creating D3D12 device...");
    if (!CreateD3D12Device()) {
        VK_ERR("[VkD3D12] Failed to create D3D12 device");
        return false;
    }
    VK_LOG("[VkD3D12] Step 1: D3D12 device created successfully");

    // 2. 创建 DirectComposition
    VK_LOG("[VkD3D12] Step 2: Creating DirectComposition...");
    if (!CreateDirectComposition()) {
        VK_ERR("[VkD3D12] Failed to create DirectComposition");
        return false;
    }
    VK_LOG("[VkD3D12] Step 2: DirectComposition created successfully");

    // 3. 创建共享纹理
    VK_LOG("[VkD3D12] Step 3: Creating shared texture...");
    if (!CreateSharedTexture(width, height)) {
        VK_ERR("[VkD3D12] Failed to create shared texture");
        return false;
    }
    VK_LOG("[VkD3D12] Step 3: Shared texture created successfully");

    // 4. 在 Vulkan 中导入共享纹理
    VK_LOG("[VkD3D12] Step 4: Importing texture to Vulkan...");
    if (!ImportToVulkan()) {
        VK_ERR("[VkD3D12] Failed to import texture to Vulkan");
        return false;
    }
    VK_LOG("[VkD3D12] Step 4: Texture imported successfully");

    VK_LOG("[VkD3D12] Vulkan-D3D12 interop initialized (" << width << "x" << height << ")");
    return true;
}

void VulkanD3D12Interop::Shutdown() {
    // 等待 GPU 完成
    if (d3d12CmdQueue_ && d3d12Fence_ && d3d12FenceEvent_) {
        d3d12FenceValue_++;
        d3d12CmdQueue_->Signal(d3d12Fence_.Get(), d3d12FenceValue_);
        if (d3d12Fence_->GetCompletedValue() < d3d12FenceValue_) {
            d3d12Fence_->SetEventOnCompletion(d3d12FenceValue_, d3d12FenceEvent_);
            WaitForSingleObject(d3d12FenceEvent_, INFINITE);
        }
    }

    // 释放 Vulkan 资源
    // 注意：必须在释放 Diligent 的 Vulkan 设备之前调用 Shutdown
    // 如果 vkNativeDevice_ 已经无效（Diligent 设备已销毁），跳过清理
    sharedRTV_ = nullptr;
    sharedSRV_ = nullptr;
    vkSharedTexture_.Release();

    // 只有当 Vulkan 设备还有效时才释放 native 资源
    if (vkNativeDevice_ != nullptr) {
        if (vkSharedMemory_) {
            vkFreeMemory(VK_DEVICE, VK_MEMORY, nullptr);
            vkSharedMemory_ = nullptr;
        }
        if (vkSharedImage_) {
            vkDestroyImage(VK_DEVICE, VK_IMAGE, nullptr);
            vkSharedImage_ = nullptr;
        }
    } else {
        // 设备已被销毁，只清空指针（资源会随设备销毁）
        vkSharedMemory_ = nullptr;
        vkSharedImage_  = nullptr;
    }
    vkNativeDevice_   = nullptr;
    vkPhysicalDevice_ = nullptr;

    // 关闭共享 handle
    if (sharedHandle_) {
        CloseHandle(sharedHandle_);
        sharedHandle_ = nullptr;
    }

    // 释放 D3D12/DirectComposition 资源
    d3d12SharedTexture_.Reset();
    for (auto& buf : swapChainBuffers_) {
        buf.Reset();
    }
    swapChain_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();

    if (d3d12FenceEvent_) {
        CloseHandle(d3d12FenceEvent_);
        d3d12FenceEvent_ = nullptr;
    }
    d3d12Fence_.Reset();
    d3d12CmdList_.Reset();
    d3d12CmdAlloc_.Reset();
    d3d12CmdQueue_.Reset();
    d3d12Device_.Reset();

    vkDevice_  = nullptr;
    vkContext_ = nullptr;
    hwnd_      = nullptr;
    width_     = 0;
    height_    = 0;
}

bool VulkanD3D12Interop::Resize(uint32_t width, uint32_t height) {
    if (!IsInitialized()) {
        return false;
    }

    if (width == width_ && height == height_) {
        return true;
    }

    // 等待 GPU 完成
    d3d12FenceValue_++;
    d3d12CmdQueue_->Signal(d3d12Fence_.Get(), d3d12FenceValue_);
    if (d3d12Fence_->GetCompletedValue() < d3d12FenceValue_) {
        d3d12Fence_->SetEventOnCompletion(d3d12FenceValue_, d3d12FenceEvent_);
        WaitForSingleObject(d3d12FenceEvent_, INFINITE);
    }

    // 释放旧资源
    sharedRTV_ = nullptr;
    sharedSRV_ = nullptr;
    vkSharedTexture_.Release();

    if (vkSharedMemory_) {
        vkFreeMemory(VK_DEVICE, VK_MEMORY, nullptr);
        vkSharedMemory_ = nullptr;
    }
    if (vkSharedImage_) {
        vkDestroyImage(VK_DEVICE, VK_IMAGE, nullptr);
        vkSharedImage_ = nullptr;
    }

    if (sharedHandle_) {
        CloseHandle(sharedHandle_);
        sharedHandle_ = nullptr;
    }
    d3d12SharedTexture_.Reset();

    for (auto& buf : swapChainBuffers_) {
        buf.Reset();
    }

    // 调整 SwapChain 大小
    HRESULT hr = swapChain_->ResizeBuffers(kBufferCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] ResizeBuffers failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 重新获取后缓冲
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers_[i]));
        if (FAILED(hr)) {
            std::cerr << "[VkD3D12] GetBuffer(" << i << ") failed" << std::endl;
            return false;
        }
    }

    width_  = width;
    height_ = height;

    // 重新创建共享纹理
    if (!CreateSharedTexture(width, height)) {
        return false;
    }

    // 重新导入到 Vulkan
    if (!ImportToVulkan()) {
        return false;
    }

    std::cout << "[VkD3D12] Resized to " << width << "x" << height << std::endl;
    return true;
}

bool VulkanD3D12Interop::Present(uint32_t syncInterval) {
    if (!IsInitialized()) {
        return false;
    }

    // 重置命令分配器和列表
    d3d12CmdAlloc_->Reset();
    d3d12CmdList_->Reset(d3d12CmdAlloc_.Get(), nullptr);

    const uint32_t  backBufferIdx = swapChain_->GetCurrentBackBufferIndex();
    ID3D12Resource* backBuffer    = swapChainBuffers_[backBufferIdx].Get();

    // 资源屏障：共享纹理 COMMON -> COPY_SOURCE
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource   = d3d12SharedTexture_.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // 资源屏障：后缓冲 PRESENT -> COPY_DEST
    barriers[1].Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource   = backBuffer;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    d3d12CmdList_->ResourceBarrier(2, barriers);

    // 复制共享纹理到后缓冲
    d3d12CmdList_->CopyResource(backBuffer, d3d12SharedTexture_.Get());

    // 资源屏障：恢复状态
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;

    d3d12CmdList_->ResourceBarrier(2, barriers);

    // 关闭并执行命令列表
    d3d12CmdList_->Close();
    ID3D12CommandList* cmdLists[] = {d3d12CmdList_.Get()};
    d3d12CmdQueue_->ExecuteCommandLists(1, cmdLists);

    // Present
    HRESULT hr = swapChain_->Present(syncInterval, 0);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] Present failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 等待 GPU 完成
    d3d12FenceValue_++;
    d3d12CmdQueue_->Signal(d3d12Fence_.Get(), d3d12FenceValue_);

    return true;
}

void VulkanD3D12Interop::ClearWithColor(float r, float g, float b, float a) {
    if (!IsInitialized() || !sharedRTV_) {
        return;
    }

    const float clearColor[] = {r, g, b, a};
    vkContext_->SetRenderTargets(1, &sharedRTV_, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    vkContext_->ClearRenderTarget(sharedRTV_, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    vkContext_->Flush();
    vkContext_->FinishFrame();
    vkContext_->WaitForIdle();
}

void VulkanD3D12Interop::ClearAllBackBuffers() {
    if (!d3d12Device_ || !d3d12CmdQueue_ || !d3d12CmdAlloc_ || !d3d12CmdList_ || !swapChain_) {
        return;
    }

    // 创建 RTV 描述符堆用于清除
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors             = kBufferCount;
    rtvHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtvHeap;
    if (FAILED(d3d12Device_->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap)))) {
        return;
    }

    const UINT rtvDescSize = d3d12Device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = rtvHeap->GetCPUDescriptorHandleForHeapStart();

    // 为每个后缓冲创建 RTV
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        d3d12Device_->CreateRenderTargetView(swapChainBuffers_[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += rtvDescSize;
    }

    // 清除每个后缓冲
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (uint32_t i = 0; i < kBufferCount; ++i) {
        d3d12CmdAlloc_->Reset();
        d3d12CmdList_->Reset(d3d12CmdAlloc_.Get(), nullptr);

        // 转换为 RENDER_TARGET 状态
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource   = swapChainBuffers_[i].Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        d3d12CmdList_->ResourceBarrier(1, &barrier);

        // 清除
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += i * rtvDescSize;
        d3d12CmdList_->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        // 转换回 PRESENT 状态
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        d3d12CmdList_->ResourceBarrier(1, &barrier);

        d3d12CmdList_->Close();
        ID3D12CommandList* cmdLists[] = {d3d12CmdList_.Get()};
        d3d12CmdQueue_->ExecuteCommandLists(1, cmdLists);

        // 等待完成
        d3d12FenceValue_++;
        d3d12CmdQueue_->Signal(d3d12Fence_.Get(), d3d12FenceValue_);
        if (d3d12Fence_->GetCompletedValue() < d3d12FenceValue_) {
            d3d12Fence_->SetEventOnCompletion(d3d12FenceValue_, d3d12FenceEvent_);
            WaitForSingleObject(d3d12FenceEvent_, INFINITE);
        }

        // Present 这一帧以推进 SwapChain
        swapChain_->Present(0, 0);
    }
}

bool VulkanD3D12Interop::CreateD3D12Device() {
    // 创建 DXGI Factory
    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    UINT                                  dxgiFlags = 0;
#ifdef _DEBUG
    dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    HRESULT hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateDXGIFactory2 failed" << std::endl;
        return false;
    }

    // 获取默认适配器
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    hr = dxgiFactory->EnumAdapters1(0, &adapter);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] EnumAdapters1 failed" << std::endl;
        return false;
    }

    // 创建 D3D12 设备
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] D3D12CreateDevice failed" << std::endl;
        return false;
    }

    // 创建命令队列
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type                     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags                    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr                                 = d3d12Device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3d12CmdQueue_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateCommandQueue failed" << std::endl;
        return false;
    }

    // 创建命令分配器
    hr = d3d12Device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3d12CmdAlloc_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateCommandAllocator failed" << std::endl;
        return false;
    }

    // 创建命令列表
    hr = d3d12Device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12CmdAlloc_.Get(), nullptr,
                                         IID_PPV_ARGS(&d3d12CmdList_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateCommandList failed" << std::endl;
        return false;
    }
    d3d12CmdList_->Close();

    // 创建 Fence
    hr = d3d12Device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateFence failed" << std::endl;
        return false;
    }

    d3d12FenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d3d12FenceEvent_) {
        std::cerr << "[VkD3D12] CreateEvent failed" << std::endl;
        return false;
    }

    std::cout << "[VkD3D12] D3D12 device created (for DirectComposition only)" << std::endl;
    return true;
}

bool VulkanD3D12Interop::CreateDirectComposition() {
    std::cout << "[VkD3D12] CreateDirectComposition: hwnd=" << hwnd_ << std::endl;

    // 创建 DirectComposition 设备
    HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcompDevice_));
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] DCompositionCreateDevice failed: hr=0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[VkD3D12] DCompositionCreateDevice: OK" << std::endl;

    // 创建 target
    hr = dcompDevice_->CreateTargetForHwnd(hwnd_, TRUE, &dcompTarget_);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateTargetForHwnd failed: hr=0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[VkD3D12] CreateTargetForHwnd: OK" << std::endl;

    // 创建 visual
    hr = dcompDevice_->CreateVisual(&dcompVisual_);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateVisual failed: hr=0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[VkD3D12] CreateVisual: OK" << std::endl;

    // 创建 SwapChain
    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width                 = width_;
    scDesc.Height                = height_;
    scDesc.Format                = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.Stereo                = FALSE;
    scDesc.SampleDesc.Count      = 1;
    scDesc.SampleDesc.Quality    = 0;
    scDesc.BufferUsage           = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount           = kBufferCount;
    scDesc.Scaling               = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect            = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode             = DXGI_ALPHA_MODE_PREMULTIPLIED;
    scDesc.Flags                 = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = dxgiFactory->CreateSwapChainForComposition(d3d12CmdQueue_.Get(), &scDesc, nullptr, &swapChain1);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateSwapChainForComposition failed" << std::endl;
        return false;
    }

    hr = swapChain1.As(&swapChain_);
    if (FAILED(hr)) {
        return false;
    }

    // 获取后缓冲
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&swapChainBuffers_[i]));
        if (FAILED(hr)) {
            std::cerr << "[VkD3D12] GetBuffer(" << i << ") failed" << std::endl;
            return false;
        }
    }

    // 设置 visual 内容
    hr = dcompVisual_->SetContent(swapChain_.Get());
    if (FAILED(hr)) {
        return false;
    }

    hr = dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(hr)) {
        return false;
    }

    hr = dcompDevice_->Commit();
    if (FAILED(hr)) {
        return false;
    }

    std::cout << "[VkD3D12] DirectComposition created" << std::endl;
    return true;
}

bool VulkanD3D12Interop::CreateSharedTexture(uint32_t width, uint32_t height) {
    // 创建共享堆属性
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type                  = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty       = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference  = D3D12_MEMORY_POOL_UNKNOWN;

    // 创建资源描述
    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension           = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Alignment           = 0;
    resDesc.Width               = width;
    resDesc.Height              = height;
    resDesc.DepthOrArraySize    = 1;
    resDesc.MipLevels           = 1;
    resDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
    resDesc.SampleDesc.Count    = 1;
    resDesc.SampleDesc.Quality  = 0;
    resDesc.Layout              = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    // 创建共享纹理
    HRESULT hr =
        d3d12Device_->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                              nullptr, IID_PPV_ARGS(&d3d12SharedTexture_));

    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateCommittedResource (shared texture) failed: 0x" << std::hex << hr << std::dec
                  << std::endl;
        return false;
    }

    // 创建共享 handle
    hr = d3d12Device_->CreateSharedHandle(d3d12SharedTexture_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedHandle_);
    if (FAILED(hr)) {
        std::cerr << "[VkD3D12] CreateSharedHandle failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[VkD3D12] Shared texture created (" << width << "x" << height << ")" << std::endl;
    return true;
}

bool VulkanD3D12Interop::ImportToVulkan() {
    // 获取 Vulkan 设备
    RefCntAutoPtr<IRenderDeviceVk> deviceVk;
    vkDevice_->QueryInterface(IID_RenderDeviceVk,
                              reinterpret_cast<IObject**>(static_cast<IRenderDeviceVk**>(&deviceVk)));
    if (!deviceVk) {
        std::cerr << "[VkD3D12] Failed to get IRenderDeviceVk" << std::endl;
        return false;
    }

    // 更新成员变量（void*）
    vkNativeDevice_   = deviceVk->GetVkDevice();
    vkPhysicalDevice_ = deviceVk->GetVkPhysicalDevice();

    // 使用宏获取 Vulkan 类型以方便使用
    VkDevice         vkNativeDevice = VK_DEVICE;
    VkPhysicalDevice vkPhysDevice   = VK_PHYS_DEVICE;

    OutputDebugStringA("[VkD3D12] Got Vulkan device, loading volk functions...\n");

    // 使用 volk 加载设备特定的 Vulkan 函数
    // 注意：Diligent 使用 volk，所以我们需要确保函数指针已加载
    volkLoadDevice(vkNativeDevice);

    OutputDebugStringA("[VkD3D12] volk loaded, getting extension function...\n");

    // 尝试加载外部内存扩展函数
    auto vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(vkNativeDevice, "vkGetMemoryWin32HandlePropertiesKHR"));

    if (!vkGetMemoryWin32HandlePropertiesKHR) {
        std::cerr << "[VkD3D12] VK_KHR_external_memory_win32 not available, falling back to independent texture"
                  << std::endl;
        return CreateFallbackTexture();
    }

    // 查询 handle 的内存属性
    VkMemoryWin32HandlePropertiesKHR handleProps = {};
    handleProps.sType                            = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;

    VkResult vkRes = vkGetMemoryWin32HandlePropertiesKHR(
        vkNativeDevice, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, sharedHandle_, &handleProps);

    if (vkRes != VK_SUCCESS) {
        std::cerr << "[VkD3D12] vkGetMemoryWin32HandlePropertiesKHR failed: " << vkRes
                  << ", falling back to independent texture" << std::endl;
        return CreateFallbackTexture();
    }

    // 创建 VkImage（支持外部内存）
    VkExternalMemoryImageCreateInfo extMemImageInfo = {};
    extMemImageInfo.sType                           = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extMemImageInfo.handleTypes                     = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;

    VkImageCreateInfo imageInfo = {};
    imageInfo.sType             = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.pNext             = &extMemImageInfo;
    imageInfo.imageType         = VK_IMAGE_TYPE_2D;
    imageInfo.format            = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent            = {width_, height_, 1};
    imageInfo.mipLevels         = 1;
    imageInfo.arrayLayers       = 1;
    imageInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage vkSharedImage = VK_NULL_HANDLE;
    vkRes                 = vkCreateImage(vkNativeDevice, &imageInfo, nullptr, &vkSharedImage);
    if (vkRes != VK_SUCCESS) {
        std::cerr << "[VkD3D12] vkCreateImage failed: " << vkRes << ", falling back to independent texture"
                  << std::endl;
        return CreateFallbackTexture();
    }
    vkSharedImage_ = (void*)vkSharedImage; // 保存 handle

    // 获取内存需求
    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(vkNativeDevice, vkSharedImage, &memReqs);

    // 导入外部内存
    VkImportMemoryWin32HandleInfoKHR importInfo = {};
    importInfo.sType                            = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importInfo.handleType                       = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    importInfo.handle                           = sharedHandle_;

    // 查找兼容的内存类型
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(vkPhysDevice, &memProps);

    uint32_t memTypeIndex = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) && (handleProps.memoryTypeBits & (1u << i))) {
            memTypeIndex = i;
            break;
        }
    }

    if (memTypeIndex == UINT32_MAX) {
        std::cerr << "[VkD3D12] No compatible memory type found, falling back to independent texture" << std::endl;
        vkDestroyImage(vkNativeDevice, vkSharedImage, nullptr);
        vkSharedImage_ = nullptr;
        return CreateFallbackTexture();
    }

    VkMemoryDedicatedAllocateInfo dedicatedInfo = {};
    dedicatedInfo.sType                         = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicatedInfo.image                         = vkSharedImage;

    importInfo.pNext = &dedicatedInfo;

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType                = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext                = &importInfo;
    allocInfo.allocationSize       = memReqs.size;
    allocInfo.memoryTypeIndex      = memTypeIndex;

    VkDeviceMemory vkSharedMemory = VK_NULL_HANDLE;
    vkRes                         = vkAllocateMemory(vkNativeDevice, &allocInfo, nullptr, &vkSharedMemory);
    if (vkRes != VK_SUCCESS) {
        std::cerr << "[VkD3D12] vkAllocateMemory failed: " << vkRes << ", falling back to independent texture"
                  << std::endl;
        vkDestroyImage(vkNativeDevice, vkSharedImage, nullptr);
        vkSharedImage_ = nullptr;
        return CreateFallbackTexture();
    }
    vkSharedMemory_ = (void*)vkSharedMemory; // 保存内存 handle

    // 绑定内存
    vkRes = vkBindImageMemory(vkNativeDevice, vkSharedImage, vkSharedMemory, 0);
    if (vkRes != VK_SUCCESS) {
        std::cerr << "[VkD3D12] vkBindImageMemory failed: " << vkRes << std::endl;
        vkFreeMemory(vkNativeDevice, vkSharedMemory, nullptr);
        vkDestroyImage(vkNativeDevice, vkSharedImage, nullptr);
        vkSharedMemory_ = nullptr;
        vkSharedImage_  = nullptr;
        return CreateFallbackTexture();
    }

    // 创建 Diligent 纹理包装
    TextureDesc texDesc;
    texDesc.Name      = "VkD3D12 Shared RT";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = width_;
    texDesc.Height    = height_;
    texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    texDesc.MipLevels = 1;
    texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;

    // 使用 Diligent 的 CreateTextureFromVulkanImage
    // vkSharedImage 已经是 VkImage 类型
    deviceVk->CreateTextureFromVulkanImage(vkSharedImage, texDesc, RESOURCE_STATE_UNKNOWN, &vkSharedTexture_);

    if (!vkSharedTexture_) {
        std::cerr << "[VkD3D12] Failed to create Diligent texture wrapper, falling back" << std::endl;
        vkFreeMemory(vkNativeDevice, vkSharedMemory, nullptr);
        vkDestroyImage(vkNativeDevice, vkSharedImage, nullptr);
        vkSharedMemory_ = nullptr;
        vkSharedImage_  = nullptr;
        return CreateFallbackTexture();
    }

    sharedRTV_ = vkSharedTexture_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    sharedSRV_ = vkSharedTexture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

    if (!sharedRTV_ || !sharedSRV_) {
        std::cerr << "[VkD3D12] Failed to get texture views" << std::endl;
        return false;
    }

    std::cout << "[VkD3D12] Vulkan external memory import successful (" << width_ << "x" << height_ << ")" << std::endl;
    return true;
}

bool VulkanD3D12Interop::CreateFallbackTexture() {
    // 回退方案：创建独立的 Vulkan 纹理
    std::cout << "[VkD3D12] Using fallback: independent Vulkan texture (slower)" << std::endl;

    TextureDesc texDesc;
    texDesc.Name      = "VkD3D12 Fallback RT";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = width_;
    texDesc.Height    = height_;
    texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    texDesc.MipLevels = 1;
    texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_DEFAULT;

    vkDevice_->CreateTexture(texDesc, nullptr, &vkSharedTexture_);
    if (!vkSharedTexture_) {
        std::cerr << "[VkD3D12] Failed to create fallback Vulkan render target" << std::endl;
        return false;
    }

    sharedRTV_ = vkSharedTexture_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    sharedSRV_ = vkSharedTexture_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);

    if (!sharedRTV_ || !sharedSRV_) {
        std::cerr << "[VkD3D12] Failed to get fallback texture views" << std::endl;
        return false;
    }

    std::cout << "[VkD3D12] Fallback Vulkan render target created" << std::endl;
    return true;
}

void VulkanD3D12Interop::FlushVulkan() {
    if (vkContext_) {
        vkContext_->Flush();
        vkContext_->WaitForIdle();
    }
}

} // namespace ParticleSaturn::Render
