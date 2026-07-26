#pragma once

#include "CocoaHost.h"
#include "MacOSMd3Panel.h"
#include "SmokeHarness.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "services/settings/macos/NSUserDefaultsStore.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ParticleSaturn::Platform::MacOS {

// ============================================================================
// 应用外壳（TODO P1：三 main 合并为 RunApp；D-002 帧高度接缝 / D-009 唯一外壳）
//
// 接缝以上（本文件）：设置持久化、相机/手势、输入动作分发、帧推进与 FPS、
// 窗口状态镜像、材质/垂直同步应用、MD3 面板内容、冒烟检查、退出码。
// 接缝以下（renderFrame 回调）：后端用最地道的原生写法渲染一帧，包括各自的
// ImGui 后端接入与基线捕获——它们是对比实验的被测对象，不做 RHI 化。
// ============================================================================

// 宿主窗口抽象：Metal/Vulkan 直接适配 CocoaHost；GL41 保留自建 NSOpenGL 窗口栈
// （材质/全屏行为不因合并而改变），以本接口接入外壳。
class AppHost {
public:
    virtual ~AppHost() = default;
    virtual DrawableSize CurrentDrawableSize() = 0;
    virtual void WindowPosition(std::int32_t& x, std::int32_t& y) = 0;
    virtual bool NativeFullscreen() = 0;
    virtual void ToggleFullscreen() = 0;                       // 应用语义切换（含背景处理）
    virtual void SetWindowMaterial(App::WindowMaterial material) = 0;
    virtual void SetPresentationMode(int vsyncMode) = 0;
    virtual void RequestExit() = 0;
    virtual void Show() = 0;
    virtual void Run(const std::function<void()>& frameCallback) = 0;
    virtual void SetActionCallback(std::function<void(HostAction)> callback) = 0;
};

// CocoaHost 直通适配（Metal / Vulkan 共用）。
class CocoaAppHost final : public AppHost {
public:
    explicit CocoaAppHost(CocoaHost& host) : host_{host} {}
    DrawableSize CurrentDrawableSize() override { return host_.CurrentDrawableSize(); }
    void WindowPosition(std::int32_t& x, std::int32_t& y) override { host_.GetWindowPosition(x, y); }
    bool NativeFullscreen() override;
    void ToggleFullscreen() override { host_.ToggleFullscreen(); }
    void SetWindowMaterial(App::WindowMaterial material) override { host_.SetWindowMaterial(material); }
    void SetPresentationMode(int vsyncMode) override { host_.SetPresentationMode(vsyncMode); }
    void RequestExit() override { host_.RequestExit(); }
    void Show() override { host_.Show(); }
    void Run(const std::function<void()>& frameCallback) override { host_.Run(frameCallback); }
    void SetActionCallback(std::function<void(HostAction)> callback) override {
        host_.SetActionCallback(std::move(callback));
    }

private:
    CocoaHost& host_;
};

// 后端在 UI 编码点补充的本帧 acrylic 纹理钩子（可为空）。
struct BackendPanelHooks {
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float)> drawAcrylicBackground;
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float)> drawGraphAcrylic;
};

// 外壳每帧交给后端 renderFrame 的共享帧描述（D-002）。
struct FrameContext {
    App::AppState& state;                 // 帧协调后的状态；窗口字段已由外壳镜像
    float deltaTime = 0.0f;
    bool handTracked = false;
    const App::GestureInput& gesture;
    std::uint32_t framesPerSecond = 60;
    DrawableSize drawableSize{};
    bool nativeFullscreen = false;
    // 后端在其 ImGui 帧内调用：外壳据此绘制 MD3 面板（标题/回调/手势状态）。
    const std::function<void(const BackendPanelHooks&)>& drawPanel;
};

struct RunAppConfig {
    AppHost& host;
    App::AppController& controller;
    Services::Settings::MacOS::NSUserDefaultsStore& settings;
    SmokeConfig smoke;
    SmokeHarness& smokeHarness;
    StartupGeometry startup;
    std::string panelTitle;               // MD3 面板抬头（按值持有：adapter 名在设备恢复时可能重建）
    bool panelSupportsAnalyticParticles = false;  // RenderMd3Panel 第四参（GL41 为 true）
    bool persistSettings = true;          // 冒烟/基线模式不读写用户设置
    bool cameraEnabled = true;            // Vulkan 帧数冒烟禁用相机
    float fixedDeltaTime = 0.0f;          // >0 时替代真实帧时（Vulkan lod 冒烟用 0.05）
    // 动作回调安装后、运行循环前调用；返回非零则中止并作为进程退出码（Vulkan
    // 交互冒烟用）。可为空。
    std::function<int()> preRun;
    // 后端一帧：返回 false 表示本帧中止（后端已自行处理失败/退出请求）。
    std::function<bool(const FrameContext&)> renderFrame;
};

// 唯一应用外壳。返回进程退出码（冒烟失败→1）。后端资源的构建与析构都在调用方
// （main）：RunApp 返回后再做各自的 ImGui/设备清理。
int RunApp(RunAppConfig& config);

} // namespace ParticleSaturn::Platform::MacOS
