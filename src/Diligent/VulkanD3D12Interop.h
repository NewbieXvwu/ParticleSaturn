#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "Texture.h"

namespace ParticleSaturn::Render {

/// Vulkan + D3D12 互操作层，实现 Vulkan 渲染 + DirectComposition 透明呈现
///
/// 工作原理：
/// 1. 创建独立的 D3D12 设备（仅用于 DirectComposition 呈现）
/// 2. 创建 D3D12 共享纹理（NT HANDLE）
/// 3. 在 Vulkan 中通过 VK_KHR_external_memory_win32 导入共享纹理
/// 4. Vulkan 渲染到共享纹理
/// 5. D3D12 复制共享纹理到 DirectComposition SwapChain 并呈现
class VulkanD3D12Interop final {
  public:
    VulkanD3D12Interop() = default;
    ~VulkanD3D12Interop();

    VulkanD3D12Interop(const VulkanD3D12Interop&)            = delete;
    VulkanD3D12Interop& operator=(const VulkanD3D12Interop&) = delete;
    VulkanD3D12Interop(VulkanD3D12Interop&&)                 = delete;
    VulkanD3D12Interop& operator=(VulkanD3D12Interop&&)      = delete;

    /// 初始化互操作层
    /// @param hwnd 目标窗口（需要 WS_EX_NOREDIRECTIONBITMAP）
    /// @param vkDevice Diligent Vulkan 设备
    /// @param vkContext Diligent Vulkan 上下文
    /// @param width 渲染目标宽度
    /// @param height 渲染目标高度
    /// @return 是否成功
    bool Init(HWND hwnd, Diligent::IRenderDevice* vkDevice, Diligent::IDeviceContext* vkContext, uint32_t width,
              uint32_t height);

    void Shutdown();

    /// 调整大小
    bool Resize(uint32_t width, uint32_t height);

    /// 获取 Vulkan 端的共享纹理（用于渲染）
    Diligent::ITexture* GetSharedTexture() const { return vkSharedTexture_; }

    /// 获取 Vulkan 端的共享纹理 RTV
    Diligent::ITextureView* GetSharedRTV() const { return sharedRTV_; }

    /// 获取 Vulkan 端的共享纹理 SRV
    Diligent::ITextureView* GetSharedSRV() const { return sharedSRV_; }

    /// 用指定颜色清空共享纹理
    void ClearWithColor(float r, float g, float b, float a);

    /// 将共享纹理内容提交到 DirectComposition SwapChain
    /// @param syncInterval VSync 间隔
    /// @return 是否成功
    bool Present(uint32_t syncInterval);

    /// 在 Vulkan 端 flush 并同步（Present 前调用）
    void FlushVulkan();

    uint32_t GetWidth() const { return width_; }

    uint32_t GetHeight() const { return height_; }

    bool IsInitialized() const { return d3d12Device_ != nullptr && vkSharedTexture_ != nullptr; }

    /// 使用 D3D12 直接清除所有 SwapChain 后缓冲（在初始化后调用，避免残留）
    void ClearAllBackBuffers();

  private:
    bool CreateD3D12Device();
    bool CreateDirectComposition();
    bool CreateSharedTexture(uint32_t width, uint32_t height);
    bool ImportToVulkan();
    bool CreateFallbackTexture();

    HWND     hwnd_   = nullptr;
    uint32_t width_  = 0;
    uint32_t height_ = 0;

    // Diligent Vulkan 设备（外部传入，不拥有）
    Diligent::IRenderDevice*  vkDevice_  = nullptr;
    Diligent::IDeviceContext* vkContext_ = nullptr;

    // 原生 Vulkan 句柄（使用 opaque 类型，避免在头文件包含 vulkan.h）
    // 实际类型: VkDevice, VkPhysicalDevice, VkQueue
    void* vkNativeDevice_   = nullptr;
    void* vkPhysicalDevice_ = nullptr;
    void* vkQueue_          = nullptr;

    // D3D12 设备（独立创建，仅用于 DirectComposition）
    Microsoft::WRL::ComPtr<ID3D12Device>              d3d12Device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        d3d12CmdQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    d3d12CmdAlloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> d3d12CmdList_;
    Microsoft::WRL::ComPtr<ID3D12Fence>               d3d12Fence_;
    HANDLE                                            d3d12FenceEvent_ = nullptr;
    uint64_t                                          d3d12FenceValue_ = 0;

    // DirectComposition
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcompVisual_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>     swapChain_;
    static constexpr uint32_t                   kBufferCount = 3;
    Microsoft::WRL::ComPtr<ID3D12Resource>      swapChainBuffers_[kBufferCount];

    // D3D12 共享纹理
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12SharedTexture_;
    HANDLE                                 sharedHandle_ = nullptr;

    // Vulkan 导入的共享纹理（使用 opaque 类型）
    // 实际类型: VkImage, VkDeviceMemory
    void* vkSharedImage_  = nullptr;
    void* vkSharedMemory_ = nullptr;

    // Diligent 包装的纹理
    Diligent::RefCntAutoPtr<Diligent::ITexture> vkSharedTexture_;
    Diligent::ITextureView*                     sharedRTV_ = nullptr;
    Diligent::ITextureView*                     sharedSRV_ = nullptr;

    // Vulkan 同步（使用 opaque 类型）
    // 实际类型: VkSemaphore, VkFence
    void* vkRenderFinishedSemaphore_ = nullptr;
    void* vkFence_                   = nullptr;
};

} // namespace ParticleSaturn::Render
