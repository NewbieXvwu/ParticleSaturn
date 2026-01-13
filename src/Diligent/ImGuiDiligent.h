#pragma once

// ImGui integration for Diligent Engine
// 基于 Diligent 抽象层实现，自动支持所有 Diligent 后端（D3D11/D3D12/Vulkan/OpenGL/Metal/WebGPU）
// 使用 imgui_impl_win32 处理 Win32 输入

#include "RenderBackend.h"
#include "PipelineState.h"
#include "Buffer.h"
#include "Texture.h"
#include "RefCntAutoPtr.hpp"

struct HWND__;
using HWND = HWND__*;

namespace Diligent {
class IRenderDevice;
class IDeviceContext;
class ISwapChain;
} // namespace Diligent

namespace ParticleSaturn::UI {

class ImGuiDiligent final {
  public:
    ImGuiDiligent() = default;
    ~ImGuiDiligent();

    ImGuiDiligent(const ImGuiDiligent&)            = delete;
    ImGuiDiligent& operator=(const ImGuiDiligent&) = delete;
    ImGuiDiligent(ImGuiDiligent&&)                 = delete;
    ImGuiDiligent& operator=(ImGuiDiligent&&)      = delete;

    // 初始化 ImGui 上下文和 Diligent 渲染资源
    // backend: 用于选择着色器语言（HLSL/GLSL）
    bool Init(HWND hwnd, Render::Backend backend, Diligent::IRenderDevice* device, Diligent::ISwapChain* swapChain);
    void Shutdown();

    // 每帧开始时调用
    void NewFrame();

    // 渲染 ImGui 绘制数据到当前 RenderTarget
    void Render(Diligent::IDeviceContext* context, Diligent::ITextureView* rtv);

    // 转发 Win32 消息给 ImGui（返回 true 表示消息已被 ImGui 处理）
    bool HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam);

    bool IsInitialized() const { return initialized_; }

  private:
    bool CreateFontTexture(Diligent::IRenderDevice* device);
    bool CreatePipelineState(Diligent::IRenderDevice* device, Diligent::ISwapChain* swapChain);
    bool CreateBuffers(Diligent::IRenderDevice* device, int vertexCount, int indexCount);

    Render::Backend backend_ = Render::Backend::D3D12;
    bool            initialized_ = false;
    HWND            hwnd_ = nullptr;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice>          device_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         pso_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>               vertexBuffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>               indexBuffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>               constantBuffer_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>              fontTexture_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView>          fontSRV_;

    int vertexBufferSize_ = 0;
    int indexBufferSize_  = 0;
};

} // namespace ParticleSaturn::UI
