#pragma once

#include "app/FrameCoordinator.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImDrawList;
struct ImVec2;

namespace ParticleSaturn::App {

// ============================================================================
// 平台中立的帧高度接缝（D-002 / D-009 / D-015 重启）。
//
// 接缝以上（外壳，各平台的 RunApp）：设置持久化、相机/手势、输入动作分发、
// 帧推进与 FPS、窗口状态镜像、材质/垂直同步应用、MD3 面板内容、冒烟检查、退出码。
// 接缝以下（后端 RenderFrame）：后端用最地道的原生写法渲染一帧，包括各自的
// ImGui 后端接入与基线捕获——它们是对比实验的被测对象，不做 RHI 化。
//
// 本头文件不依赖任何平台类型（无 Cocoa / 无 Win32），macOS 与 Windows 外壳
// 共用之；平台专有的窗口尺寸/缩放统一以下面的 SurfaceSize 表达。
// ============================================================================

// 后端可绘制表面尺寸（中立化自 macOS DrawableSize；Windows 侧亦以此表达，
// scale 承载 DPI 缩放，非 Retina 平台默认 1.0）。
struct SurfaceSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float scale = 1.0f;
};

// 后端能力申报（接缝的 Capabilities 面）：特性开关只在外壳一处按
// "能力 ∧ 用户设置" 解析，面板按能力显隐；故意的实验分歧（D-004）在此登记，
// RunApp 启动时发布到 DiagnosticBus 留档。
struct BackendCapabilities {
    bool analyticParticles = false;      // 解析式粒子双策略（GL41）
    bool objectShaderParticles = false;  // object/mesh shader 粒子路径（Metal 3）
    std::vector<std::string> declaredDivergences;
};

// 后端在 UI 编码点补充的本帧 acrylic 纹理钩子（可为空）。
struct BackendPanelHooks {
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float)> drawAcrylicBackground;
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float)> drawGraphAcrylic;
};

// 外壳每帧交给后端 RenderFrame 的共享帧描述（D-002）。
struct FrameContext {
    AppState& state;                      // 帧协调后的状态；窗口字段已由外壳镜像
    float deltaTime = 0.0f;
    bool handTracked = false;
    const GestureInput& gesture;
    std::uint32_t framesPerSecond = 60;
    SurfaceSize drawableSize{};
    bool nativeFullscreen = false;
    // 后端在其 ImGui 帧内调用：外壳据此绘制 MD3 面板（标题/回调/手势状态）。
    const std::function<void(const BackendPanelHooks&)>& drawPanel;
};

// D-002 帧高度接缝的命名接口：后端接入外壳的完整契约。
// 接缝以上（外壳）只见此接口；接缝以下各后端保持最地道的原生实现。
// Init/Resize/Shutdown 留在各后端的对象生命周期里（构造前 / RenderFrame 内 /
// RunApp 返回后），不强行入接口——外壳无须驱动它们。
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    // 能力申报与声明分歧（D-004）：外壳按"能力 ∧ 用户设置"解析并发布留档。
    virtual const BackendCapabilities& Capabilities() const = 0;
    // 后端一帧：返回 false 表示本帧中止（后端已自行处理失败/退出请求）。
    virtual bool RenderFrame(const FrameContext& frame) = 0;
    // P4 读回面（Readback）：确定性基线是否已写盘。捕获机制保持原生
    //（GL glReadPixels / Metal blit+fp16 / Vulkan staging 拷贝，均在各自帧
    // 管线的正确挂点），外壳在冒烟捕获模式下据此收束运行循环。
    virtual bool BaselineCaptured() const = 0;
};

} // namespace ParticleSaturn::App
