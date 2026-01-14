#pragma once

// ImGui integration for Diligent Engine
// 基于 Diligent 抽象层实现，自动支持所有 Diligent 后端（D3D11/D3D12/Vulkan/OpenGL/Metal/WebGPU）
// 使用 imgui_impl_win32 处理 Win32 输入

#include "Buffer.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderBackend.h"
#include "Texture.h"

struct HWND__;
using HWND = HWND__*;

namespace Diligent {
class IRenderDevice;
class IDeviceContext;
class ISwapChain;
} // namespace Diligent

namespace ParticleSaturn::UI {

// Stencil 模式枚举
enum class StencilMode {
    Disabled,  // 正常渲染，无 Stencil
    WriteIncr, // 写入 Stencil（递增），禁止颜色写入
    WriteDecr, // 写入 Stencil（递减），禁止颜色写入
    TestEqual, // Stencil 测试，相等时通过
};

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

    // Stencil 圆角裁剪 API
    void SetStencilMode(StencilMode mode, int stencilRef = 0);

    StencilMode GetStencilMode() const { return stencilMode_; }

    int GetStencilRef() const { return stencilRef_; }

    // 获取当前 DeviceContext（供回调使用）
    Diligent::IDeviceContext* GetCurrentContext() const { return currentContext_; }

    Diligent::ITextureView* GetCurrentRTV() const { return currentRTV_; }

  private:
    bool CreateFontTexture(Diligent::IRenderDevice* device);
    bool CreatePipelineStates(Diligent::IRenderDevice* device, Diligent::ISwapChain* swapChain);
    bool CreateDepthStencilBuffer(Diligent::IRenderDevice* device, Diligent::ISwapChain* swapChain);
    bool CreateBuffers(Diligent::IRenderDevice* device, int vertexCount, int indexCount);
    void ApplyStencilMode(Diligent::IDeviceContext* context);

    Render::Backend backend_     = Render::Backend::D3D12;
    bool            initialized_ = false;
    HWND            hwnd_        = nullptr;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice>          device_;
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         pso_;             // 普通 PSO（无 Stencil）
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         psoStencilWrite_; // Stencil 写入 PSO
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         psoStencilTest_;  // Stencil 测试 PSO
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srb_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srbStencilWrite_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> srbStencilTest_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                vertexBuffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                indexBuffer_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                constantBuffer_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>               fontTexture_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView>           fontSRV_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>               depthStencilTexture_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView>           dsv_;

    int vertexBufferSize_ = 0;
    int indexBufferSize_  = 0;

    // Stencil 状态
    StencilMode stencilMode_ = StencilMode::Disabled;
    int         stencilRef_  = 0;

    // 当前渲染上下文（供回调访问）
    Diligent::IDeviceContext* currentContext_ = nullptr;
    Diligent::ITextureView*   currentRTV_     = nullptr;

    // 缓存的 SwapChain 尺寸
    unsigned int cachedWidth_  = 0;
    unsigned int cachedHeight_ = 0;
};

// 全局 ImGuiDiligent 实例访问（供 MD3 回调使用）
ImGuiDiligent* GetImGuiDiligentInstance();
void           SetImGuiDiligentInstance(ImGuiDiligent* instance);

} // namespace ParticleSaturn::UI
