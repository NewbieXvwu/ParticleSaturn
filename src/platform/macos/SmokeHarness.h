#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "app/state/AppStates.h"

namespace ParticleSaturn::Platform::MacOS {

// 冒烟/基线模式统一支撑（AUDIT P2-2：从三个 main 原样收拢）。
// 解析 PARTICLESATURN_CAPTURE_BASELINE / PARTICLESATURN_PERFORMANCE_LOCK_SMOKE /
// PARTICLESATURN_FULLSCREEN_RESTORE_SMOKE 并把初始状态钉死为确定性配置。
// Vulkan 专属冒烟（SMOKE_FRAMES / INTERACTION / LOD / DEVICE_LOST / RESTART）
// 是该后端的实验变量，仍留在 VulkanMain。
struct SmokeConfig {
    const char*   baselinePath      = nullptr;
    bool          captureBaseline   = false;
    std::uint32_t performanceFrames = 0;
    bool          performanceSmoke  = false;
    std::uint32_t fullscreenFrames  = 0;
    bool          fullscreenSmoke   = false;

    static SmokeConfig FromEnvironment();

    // 任一冒烟/基线模式生效时不读也不写用户设置。
    bool Deterministic() const noexcept { return captureBaseline || performanceSmoke || fullscreenSmoke; }

    // 冒烟模式把初始状态钉死（性能锁全开、全屏恢复几何等），保证可比性。
    void ForceInitialState(App::AppState& state) const;
};

// 由（可能已被钉死的）初始状态推导窗口启动几何：全屏恢复时以窗口化几何启动。
struct StartupGeometry {
    bool          restoreFullscreen = false;
    std::uint32_t width             = 0;
    std::uint32_t height            = 0;
    std::int32_t  x                 = 0;
    std::int32_t  y                 = 0;
};

StartupGeometry ResolveStartupGeometry(const App::AppState& state);

// 逐帧冒烟检查状态机。宿主窗口操作经回调注入：Metal/Vulkan 传 CocoaHost 的
// 操作，GL41 传原生 NSWindow 包装。失败统一打 "[smoke] FAILED" 标记（ctest
// FAIL_REGULAR_EXPRESSION 判负）并发布 DiagnosticBus，随后请求停止运行循环，
// 让 main 的失败退出码真实传播（D-008）。
class SmokeHarness {
  public:
    struct HostOps {
        std::function<void()> toggleFullscreen; // 原生全屏切换
        std::function<void()> requestExit;      // 停止运行循环
    };

    // backendDomain 同时用作 DiagnosticBus 域与失败消息里的路径名（如 "metal"）。
    SmokeHarness(const SmokeConfig& config, const StartupGeometry& startup, std::string backendDomain, HostOps ops);

    // performance 冒烟：质量锁状态被改动即失败；帧数够即正常退出。
    void TickPerformance(const App::AppState& state);
    // fullscreen 冒烟：进入全屏→退出→几何恢复三段状态机，任一阶段超时即失败。
    void TickFullscreen(bool nativeFullscreen, const App::AppState& state, std::uint32_t logicalWidth,
                        std::uint32_t logicalHeight, std::int32_t windowX, std::int32_t windowY);

    bool Failed() const noexcept { return performanceFailed_ || fullscreenFailed_; }

  private:
    void Fail(bool& flag, const char* code, const char* what);

    SmokeConfig                           config_;
    StartupGeometry                       startup_;
    std::string                           backendDomain_;
    HostOps                               ops_;
    std::uint32_t                         performanceFrameCount_   = 0;
    bool                                  performanceFailed_       = false;
    std::uint32_t                         fullscreenFrameCount_    = 0;
    bool                                  fullscreenFailed_        = false;
    bool                                  fullscreenExitRequested_ = false;
    std::chrono::steady_clock::time_point fullscreenDeadline_;
};

} // namespace ParticleSaturn::Platform::MacOS
