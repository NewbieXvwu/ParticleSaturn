#import <Cocoa/Cocoa.h>

#include "AppShell.h"
#include "CocoaHost.h"
#include "MacOSApplication.h"
#include "SmokeHarness.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/diligent/DiligentVulkanAdapter.h"
#include "imgui.h"
#include "MacOSMd3Panel.h"
#include "MD3.h"
#include "services/diagnostics/DiagnosticBus.h"
#include "services/camera/macos/AVFoundationCamera.h"
#include "services/camera/macos/CameraSelectorWindow.h"
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"
#include "services/resources/macos/BundleResources.h"
#include "services/settings/macos/NSUserDefaultsStore.h"
#include "services/vulkan/VulkanDriverRuntime.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace {

ParticleSaturn::App::VulkanDriver SelectedDriver(ParticleSaturn::App::VulkanDriver saved) {
    const char* overrideValue = std::getenv("PARTICLESATURN_VULKAN_DRIVER");
    if (overrideValue == nullptr || overrideValue[0] == '\0') return saved;
    const std::string value{overrideValue};
    if (value == "molten" || value == "moltenvk") return ParticleSaturn::App::VulkanDriver::MoltenVK;
    if (value == "kosmic" || value == "kosmickrisp") return ParticleSaturn::App::VulkanDriver::KosmicKrisp;
    return saved;
}

std::uint32_t SmokeFrameLimit() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_SMOKE_FRAMES");
    if (value == nullptr || value[0] == '\0') return 0;
    const auto parsed = std::strtoul(value, nullptr, 10);
    return static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 10000UL));
}

bool InteractionSmokeRequested() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_INTERACTION_SMOKE");
    return value != nullptr && std::string_view{value} == "1";
}

bool LodSmokeRequested() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_LOD_SMOKE");
    return value != nullptr && std::string_view{value} == "1";
}

bool RestartSmokeRequested() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_RESTART_SMOKE");
    return value != nullptr && std::string_view{value} == "1";
}

bool RestartSmokeChild() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_RESTART_CHILD");
    return value != nullptr && std::string_view{value} == "1";
}

int ReportStartupFailure(const char* code, const std::string& message) {
    ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Publish(
        "vulkan", code, message, ParticleSaturn::Services::Diagnostics::Severity::Error);
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int ParticleSaturn::Platform::MacOS::RunVulkanApplication() {
    @autoreleasepool {
        InstallDebugLogCapture();
        Services::Settings::MacOS::NSUserDefaultsStore settings;
        const auto smoke = SmokeConfig::FromEnvironment();
        const char* baselinePath = smoke.baselinePath;
        const bool captureBaseline = smoke.captureBaseline;
        const bool performanceSmoke = smoke.performanceSmoke;
        const bool fullscreenSmoke = smoke.fullscreenSmoke;
        const auto smokeFrames = SmokeFrameLimit();
        const bool smokeMode = smokeFrames != 0;
        const bool lodSmoke = LodSmokeRequested();
        App::AppState defaults;
        defaults.render.graphicsApi = App::GraphicsApi::Vulkan;
        auto state = smoke.Deterministic() || smokeMode ? defaults : settings.Load(defaults);
        state.render.graphicsApi = App::GraphicsApi::Vulkan;
        state.render.vulkanDriver = SelectedDriver(state.render.vulkanDriver);
        smoke.ForceInitialState(state);
        // lod 冒烟在共享钉死之后解锁 LOD；不支持与 performance/baseline 冒烟组合使用。
        if (lodSmoke) {
            state.render.particleCount = App::RenderSettings::MaxParticles;
            state.lod.locked = false;
        }
        const auto startup = ResolveStartupGeometry(state);
        const bool restoreFullscreen = startup.restoreFullscreen;

        CocoaHost host{startup.width, startup.height, "Particle Saturn - Vulkan"};
        host.SetWindowPosition(startup.x, startup.y);
        SmokeHarness smokeHarness{smoke, startup, "vulkan", {
            [&host] { [[(NSView*)host.NativeView() window] toggleFullScreen:nil]; },
            [&host] { host.RequestExit(); }}};

        const char* resourcesPath = [[[NSBundle mainBundle] resourcePath] fileSystemRepresentation];
        if (resourcesPath == nullptr) return ReportStartupFailure("resources", "Vulkan bundle resources are unavailable");
        Gpu::Diligent::DiligentVulkanAdapter adapter;
        std::string error;
        if (!adapter.Initialize(state.render.vulkanDriver, resourcesPath, error)) {
            return ReportStartupFailure("device", error);
        }
        auto drawableSize = host.CurrentDrawableSize();
        if (!adapter.CreateSwapChain(host.NativeView(), drawableSize.width, drawableSize.height, error)) {
            return ReportStartupFailure("swap-chain", error);
        }
        if (!adapter.InitializeImGui(host.NativeView(), error)) {
            return ReportStartupFailure("imgui", error);
        }
        if (RestartSmokeRequested() && !RestartSmokeChild()) {
            const NSArray<NSString*>* processArguments = [[NSProcessInfo processInfo] arguments];
            if ([processArguments count] == 0) {
                adapter.Shutdown();
                return ReportStartupFailure("restart-smoke", "Vulkan restart executable is unavailable");
            }
            const std::string executable{[[processArguments objectAtIndex:0] fileSystemRepresentation]};
            std::vector<std::string> arguments;
            for (NSUInteger index = 1; index < [processArguments count]; ++index) {
                arguments.emplace_back([[processArguments objectAtIndex:index] UTF8String]);
            }
            adapter.Shutdown();
            if (setenv("PARTICLESATURN_VULKAN_RESTART_CHILD", "1", 1) != 0 ||
                setenv("PARTICLESATURN_VULKAN_SMOKE_FRAMES", "3", 1) != 0 ||
                setenv("PARTICLESATURN_VULKAN_INTERACTION_SMOKE", "1", 1) != 0 ||
                setenv("PARTICLESATURN_VULKAN_LOD_SMOKE", "1", 1) != 0) {
                return ReportStartupFailure("restart-smoke-env", "Vulkan restart smoke environment setup failed");
            }
            int childProcess = 0;
            std::string restartError;
            if (!Services::Vulkan::RestartWithDriver(state.render.vulkanDriver, resourcesPath, executable,
                                                     arguments, childProcess, restartError)) {
                return ReportStartupFailure("restart-smoke-spawn", restartError);
            }
            int childStatus = 0;
            if (waitpid(childProcess, &childStatus, 0) != childProcess || !WIFEXITED(childStatus) ||
                WEXITSTATUS(childStatus) != 0) {
                return ReportStartupFailure("restart-smoke-child", "Restarted Vulkan application failed");
            }
            return 0;
        }
        MD3::Init();
        MD3::SetDarkMode(state.ui.darkMode);

        App::AppController controller{state};
        std::uint32_t renderedFrames = 0;
        bool runtimeFailed = false;

        CocoaAppHost appHost{host};
        RunAppConfig shellConfig{
            appHost, controller, settings, smoke, smokeHarness, startup,
            adapter.AdapterName(), false,
            /*persistSettings=*/!(smoke.Deterministic() || smokeMode),
            /*cameraEnabled=*/!(smoke.Deterministic() || smokeMode),
            /*fixedDeltaTime=*/lodSmoke ? 0.05f : 0.0f, {}, {}};
        shellConfig.preRun = [&]() -> int {
            if (!InteractionSmokeRequested()) return 0;
            const bool debugBefore = controller.State().ui.showDebugWindow;
            const bool blurBefore = controller.State().ui.blurEnabled;
            host.InvokeAction(HostAction::KeyF3Down);
            host.InvokeAction(HostAction::KeyF3Up);
            host.InvokeAction(HostAction::KeyBDown);
            host.InvokeAction(HostAction::KeyBUp);
            if (controller.State().ui.showDebugWindow == debugBefore ||
                controller.State().ui.blurEnabled == blurBefore ||
                controller.State().input.keyF3Pressed || controller.State().input.keyBPressed) {
                adapter.Shutdown();
                return ReportStartupFailure("interaction-smoke", "Vulkan shortcut dispatch smoke test failed");
            }
            return 0;
        };
        shellConfig.renderFrame = [&](const FrameContext& frame) {
            if (frame.drawableSize.width != drawableSize.width || frame.drawableSize.height != drawableSize.height) {
                if (!adapter.ResizeSwapChain(frame.drawableSize.width, frame.drawableSize.height)) {
                    ReportStartupFailure("resize", "Vulkan swap chain resize failed");
                    runtimeFailed = true;
                    host.RequestExit();
                    return false;
                }
                drawableSize = frame.drawableSize;
            }
            const auto syncInterval = frame.state.render.vsyncMode == 0 ? 0U : 1U;
            adapter.SetParticleSettings(frame.state.render.particleCount, frame.state.scene.paused);
            adapter.SetGestureState(frame.gesture.tracked, frame.gesture.scale);
            adapter.SetSceneSettings(frame.state.scene, frame.state.render);
            adapter.SetFramesPerSecond(frame.framesPerSecond);
            adapter.SetAcrylicSettings(frame.state.ui.blurEnabled, frame.state.ui.blurStrength,
                                       frame.state.ui.darkMode);
            adapter.BeginImGuiFrame();
            MD3::BeginFrame(1.0f / 60.0f);
            MD3::SetDpiScale(1.0f);
            MD3::SetScreenSize(static_cast<float>(frame.state.window.width),
                               static_cast<float>(frame.state.window.height));
            BackendPanelHooks hooks;
            hooks.drawAcrylicBackground = [&](ImDrawList*, const ImVec2& position, const ImVec2& size, float) {
                adapter.SetAcrylicPanelRect(position.x, position.y, size.x, size.y, frame.state.window.dpiScale);
            };
            if (void* weakBlurTexture = adapter.UiWeakBlurImGuiTexture()) {
                hooks.drawGraphAcrylic = [&, weakBlurTexture](ImDrawList* drawList, const ImVec2& position,
                                                              const ImVec2& regionSize, float rounding) {
                    const float screenWidth = std::max(1.0f, static_cast<float>(frame.state.window.width));
                    const float screenHeight = std::max(1.0f, static_cast<float>(frame.state.window.height));
                    MD3::AddImageRounded(drawList, weakBlurTexture, position,
                                         ImVec2(position.x + regionSize.x, position.y + regionSize.y),
                                         ImVec2(position.x / screenWidth, position.y / screenHeight),
                                         ImVec2((position.x + regionSize.x) / screenWidth,
                                                (position.y + regionSize.y) / screenHeight),
                                         IM_COL32_WHITE, rounding);
                };
            }
            frame.drawPanel(hooks);
            MD3::EndFrame();
            if (!adapter.PresentSceneFrame(syncInterval)) {
                if (adapter.DeviceLost()) {
                    Services::Diagnostics::DiagnosticBus::Instance().Publish(
                        "vulkan", "device-recovery", "Recreating Vulkan device after device loss",
                        Services::Diagnostics::Severity::Warning);
                    adapter.Shutdown();
                    std::string recoveryError;
                    if (adapter.Initialize(frame.state.render.vulkanDriver, resourcesPath, recoveryError) &&
                        adapter.CreateSwapChain(host.NativeView(), drawableSize.width, drawableSize.height, recoveryError) &&
                        adapter.InitializeImGui(host.NativeView(), recoveryError)) {
                        Services::Diagnostics::DiagnosticBus::Instance().Publish(
                            "vulkan", "device-recovered", "Vulkan device recreation completed",
                            Services::Diagnostics::Severity::Info);
                        return false;
                    }
                    ReportStartupFailure("device-recovery", recoveryError.empty()
                        ? "Vulkan device recreation failed" : recoveryError);
                    runtimeFailed = true;
                    host.RequestExit();
                    return false;
                }
                ReportStartupFailure("present", "Vulkan frame presentation failed");
                runtimeFailed = true;
                host.RequestExit();
                return false;
            }
            if (smoke.captureBaseline && adapter.BaselineCaptureRequested()) host.RequestExit();
            if (smokeFrames != 0 && ++renderedFrames >= smokeFrames) {
                if (lodSmoke && controller.State().render.particleCount >= App::RenderSettings::MaxParticles) {
                    ReportStartupFailure("lod-smoke", "[smoke] FAILED: Vulkan dynamic LOD smoke test did not reduce particle count");
                    runtimeFailed = true;
                }
                host.RequestExit();
            }
            return true;
        };
        const int shellExit = RunApp(shellConfig);
        adapter.Shutdown();
        if (runtimeFailed) return 1;
        return shellExit;
    }
}
