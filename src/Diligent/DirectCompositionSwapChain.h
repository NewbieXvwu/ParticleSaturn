#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <dcomp.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "RenderBackend.h"

namespace ParticleSaturn::Render {

/// 使用 DirectComposition 创建透明 SwapChain（DXGI_ALPHA_MODE_PREMULTIPLIED）。
///
/// 背景：
/// - D3D11/D3D12/Vulkan 的 CreateSwapChainForHwnd 不支持 DXGI_ALPHA_MODE_PREMULTIPLIED
/// - 要实现透明窗口与 DWM 合成，必须使用 CreateSwapChainForComposition
/// - 窗口需要 WS_EX_NOREDIRECTIONBITMAP 扩展样式
///
/// 此类同时支持 D3D11 和 D3D12 后端。
class DirectCompositionSwapChain final {
  public:
    DirectCompositionSwapChain() = default;
    ~DirectCompositionSwapChain();

    DirectCompositionSwapChain(const DirectCompositionSwapChain&)            = delete;
    DirectCompositionSwapChain& operator=(const DirectCompositionSwapChain&) = delete;
    DirectCompositionSwapChain(DirectCompositionSwapChain&&)                 = delete;
    DirectCompositionSwapChain& operator=(DirectCompositionSwapChain&&)      = delete;

    /// 初始化 DirectComposition 和 SwapChain（D3D12 版本）
    /// @param hwnd 目标窗口（必须使用 WS_EX_NOREDIRECTIONBITMAP 创建）
    /// @param d3d12Device D3D12 设备
    /// @param d3d12CmdQueue D3D12 命令队列
    /// @param width SwapChain 宽度
    /// @param height SwapChain 高度
    /// @param bufferCount 后缓冲数量
    /// @return 是否成功
    bool InitD3D12(HWND hwnd, ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12CmdQueue, uint32_t width,
                   uint32_t height, uint32_t bufferCount);

    /// 初始化 DirectComposition 和 SwapChain（D3D11 版本）
    /// @param hwnd 目标窗口（必须使用 WS_EX_NOREDIRECTIONBITMAP 创建）
    /// @param d3d11Device D3D11 设备
    /// @param width SwapChain 宽度
    /// @param height SwapChain 高度
    /// @param bufferCount 后缓冲数量
    /// @return 是否成功
    bool InitD3D11(HWND hwnd, ID3D11Device* d3d11Device, uint32_t width, uint32_t height, uint32_t bufferCount);

    void Shutdown();

    /// 调整 SwapChain 大小
    bool Resize(uint32_t width, uint32_t height);

    /// 呈现
    HRESULT Present(uint32_t syncInterval);

    /// 获取当前后缓冲索引
    uint32_t GetCurrentBackBufferIndex() const;

    /// 获取后缓冲资源（D3D12）
    ID3D12Resource* GetBackBufferD3D12(uint32_t index) const;

    /// 获取后缓冲资源（D3D11）
    ID3D11Texture2D* GetBackBufferD3D11(uint32_t index) const;

    /// 获取底层 SwapChain
    IDXGISwapChain3* GetSwapChain() const { return swapChain_.Get(); }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetBufferCount() const { return bufferCount_; }

    bool IsInitialized() const { return swapChain_ != nullptr; }

    Backend GetBackendType() const { return backendType_; }

  private:
    bool InitCommon(HWND hwnd, IUnknown* deviceOrQueue, uint32_t width, uint32_t height, uint32_t bufferCount,
                    Backend backend);

    Microsoft::WRL::ComPtr<IDCompositionDevice>  dcompDevice_;
    Microsoft::WRL::ComPtr<IDCompositionTarget>  dcompTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  dcompVisual_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>      swapChain_;

    // D3D12 后缓冲
    Microsoft::WRL::ComPtr<ID3D12Resource>       d3d12BackBuffers_[3]; // 最多 3 缓冲

    // D3D11 后缓冲
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      d3d11BackBuffers_[3]; // 最多 3 缓冲

    // D3D11 专用：用于跟踪当前后缓冲索引（因为 D3D11 没有 GetCurrentBackBufferIndex）
    uint32_t d3d11CurrentBackBufferIndex_ = 0;

    HWND     hwnd_        = nullptr;
    uint32_t width_       = 0;
    uint32_t height_      = 0;
    uint32_t bufferCount_ = 0;
    Backend  backendType_ = Backend::D3D12;
};

} // namespace ParticleSaturn::Render
