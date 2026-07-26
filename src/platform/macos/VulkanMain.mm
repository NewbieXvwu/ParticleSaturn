#import <Cocoa/Cocoa.h>

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
        host.SetPresentationMode(state.render.vsyncMode);
        host.SetWindowMaterial(state.window.material);

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
        App::FrameCoordinator coordinator;
        auto lastFrame = std::chrono::steady_clock::now();
        double smoothedFrameSeconds = 1.0 / 60.0;
        std::uint32_t currentFramesPerSecond = 60;
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
        Services::Camera::MacOS::AVFoundationCamera camera;
        Services::Camera::MacOS::CameraSelectorWindow cameraSelector{camera};
        Services::HandTracking::MacOS::XnnpackHandTrackingRuntime handTrackingRuntime;
        std::unique_ptr<Services::HandTracking::MacOS::HandTrackingWorker> handTracking;
        std::string handTrackingError;
        std::uint32_t lastCameraFrameWidth = 0;
        std::uint32_t lastCameraFrameHeight = 0;
        if (!captureBaseline && !smokeMode && !performanceSmoke && !fullscreenSmoke) {
            cameraSelector.StartSaved();
            const auto palmModel = Services::Resources::MacOS::LocateModel("palm_detection_full.tflite");
            const auto landmarkModel = Services::Resources::MacOS::LocateModel("hand_landmark_full.tflite");
            if (!handTrackingRuntime.Load(palmModel, landmarkModel, handTrackingError)) {
                std::clog << "[HandTracking] " << handTrackingError << '\n';
            } else {
                handTracking = std::make_unique<Services::HandTracking::MacOS::HandTrackingWorker>(
                    [&handTrackingRuntime](const Services::Camera::Frame& frame,
                                           Services::HandTracking::MacOS::HandPose& pose,
                                           std::string& invocationError) {
                        return handTrackingRuntime.Invoke(frame, invocationError) &&
                               handTrackingRuntime.DecodeLandmarks(pose);
                    });
                handTracking->Start();
            }
        }
#endif
        host.SetActionCallback([&](HostAction action) {
            switch (action) {
            case HostAction::ToggleDebugWindow:
                controller.Dispatch(App::ToggleDebugWindow{});
                break;
            case HostAction::ToggleBlur:
                controller.Dispatch(App::SetBlurEnabled{!controller.State().ui.blurEnabled});
                break;
            case HostAction::TogglePause:
                controller.Dispatch(App::TogglePause{});
                break;
            case HostAction::ToggleFullscreen: {
                const auto effect = controller.Dispatch(App::SetFullscreen{!controller.State().window.fullscreen});
                if (effect.windowChanged) host.ToggleFullscreen();
                break;
            }
            case HostAction::NativeFullscreenEntered:
                controller.Dispatch(App::SetFullscreen{true});
                break;
            case HostAction::NativeFullscreenExited:
                controller.Dispatch(App::SetFullscreen{false});
                break;
            case HostAction::ShowCameraSelector:
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
                if (!smokeMode) {
                    if (camera.Permission() == Services::Camera::Authorization::NotDetermined) {
                        camera.RequestPermission();
                    }
                    cameraSelector.Show();
                }
#endif
                break;
            case HostAction::KeyF3Down:
            case HostAction::KeyF3Up:
            case HostAction::KeyF11Down:
            case HostAction::KeyF11Up:
            case HostAction::KeyBDown:
            case HostAction::KeyBUp:
            case HostAction::KeyEscapeDown:
            case HostAction::KeyEscapeUp: {
                const bool pressed = action == HostAction::KeyF3Down || action == HostAction::KeyF11Down ||
                    action == HostAction::KeyBDown || action == HostAction::KeyEscapeDown;
                const auto key = (action == HostAction::KeyF3Down || action == HostAction::KeyF3Up)
                    ? App::InputKey::F3
                    : (action == HostAction::KeyF11Down || action == HostAction::KeyF11Up)
                    ? App::InputKey::F11
                    : (action == HostAction::KeyBDown || action == HostAction::KeyBUp)
                    ? App::InputKey::B
                    : App::InputKey::Escape;
                const auto effect = controller.Dispatch(App::SetInputKeyPressed{key, pressed});
                if (effect.windowChanged) host.ToggleFullscreen();
                if (effect.exitRequested) host.RequestExit();
                break;
            }
            default:
                break;
            }
            if (!smokeMode && !performanceSmoke && !fullscreenSmoke) settings.Save(controller.State());
        });
        if (InteractionSmokeRequested()) {
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
        }

        std::uint32_t renderedFrames = 0;
        bool runtimeFailed = false;
        auto appliedVsync = state.render.vsyncMode;
        host.Show();
        if (restoreFullscreen) host.ToggleFullscreen();
        host.Run([&] {
            const auto now = std::chrono::steady_clock::now();
            const double elapsedSeconds = lodSmoke ? 0.05 : std::clamp(
                std::chrono::duration<double>(now - lastFrame).count(), 0.0, 0.25);
            lastFrame = now;
            if (elapsedSeconds > 0.0) {
                smoothedFrameSeconds += (elapsedSeconds - smoothedFrameSeconds) * 0.1;
                currentFramesPerSecond = static_cast<std::uint32_t>(std::clamp(
                    1.0 / smoothedFrameSeconds + 0.5, 1.0, 999.0));
            }
            const auto currentSize = host.CurrentDrawableSize();
            if (currentSize.width != drawableSize.width || currentSize.height != drawableSize.height) {
                if (!adapter.ResizeSwapChain(currentSize.width, currentSize.height)) {
                    ReportStartupFailure("resize", "Vulkan swap chain resize failed");
                    host.RequestExit();
                    return;
                }
                drawableSize = currentSize;
            }
            auto& mutableState = controller.MutableState();
            mutableState.window.width = static_cast<std::uint32_t>(drawableSize.width / drawableSize.scale);
            mutableState.window.height = static_cast<std::uint32_t>(drawableSize.height / drawableSize.scale);
            mutableState.window.dpiScale = drawableSize.scale;
            const bool nativeFullscreen = ([[(NSView*)host.NativeView() window] styleMask] &
                                           NSWindowStyleMaskFullScreen) != 0;
            if (!nativeFullscreen) {
                host.GetWindowPosition(mutableState.window.x, mutableState.window.y);
                mutableState.window.windowedX = mutableState.window.x;
                mutableState.window.windowedY = mutableState.window.y;
                mutableState.window.windowedWidth = mutableState.window.width;
                mutableState.window.windowedHeight = mutableState.window.height;
            }
            if (mutableState.render.vsyncMode != appliedVsync) {
                host.SetPresentationMode(mutableState.render.vsyncMode);
                appliedVsync = mutableState.render.vsyncMode;
            }
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
            App::GestureInput gesture;
            Services::Camera::Frame cameraFrame;
            if (handTracking && camera.LatestFrame(cameraFrame)) {
                lastCameraFrameWidth = cameraFrame.width;
                lastCameraFrameHeight = cameraFrame.height;
                handTracking->Submit(std::move(cameraFrame), controller.State().gesture.handLostDelay);
            }
            if (handTracking) gesture = handTracking->LatestGesture();
            coordinator.Advance(controller, elapsedSeconds, gesture);
#else
            App::GestureInput gesture;
            coordinator.Advance(controller, elapsedSeconds);
#endif
            Md3PanelHandTrackingStatus handStatus;
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
            using TrackerState = Md3PanelHandTrackingStatus::Tracker;
            if (!handTracking) {
                handStatus.tracker = TrackerState::Failed;
                handStatus.errorMessage = handTrackingError.empty() ? "Hand tracking runtime unavailable"
                                                                    : handTrackingError;
            } else {
                switch (camera.Permission()) {
                case Services::Camera::Authorization::Authorized:
                    handStatus.tracker = camera.IsRunning() ? TrackerState::Ready : TrackerState::Initializing;
                    break;
                case Services::Camera::Authorization::NotDetermined:
                    handStatus.tracker = TrackerState::Initializing;
                    break;
                default:
                    handStatus.tracker = TrackerState::Failed;
                    handStatus.errorMessage = "Camera access denied";
                    break;
                }
                if (lastCameraFrameWidth > 0 && lastCameraFrameHeight > 0) {
                    char cameraInfo[64];
                    std::snprintf(cameraInfo, sizeof(cameraInfo), "%u x %u", lastCameraFrameWidth,
                                  lastCameraFrameHeight);
                    handStatus.cameraInfo = cameraInfo;
                }
                handStatus.handDetected = gesture.tracked;
                handStatus.rawScale = gesture.scale;
                handStatus.rawRotX = gesture.rotationXNormalized;
                handStatus.rawRotY = gesture.rotationYNormalized;
            }
#endif
            const auto syncInterval = mutableState.render.vsyncMode == 0 ? 0U : 1U;
            adapter.SetParticleSettings(mutableState.render.particleCount, mutableState.scene.paused);
            adapter.SetGestureState(gesture.tracked, gesture.scale);
            adapter.SetSceneSettings(mutableState.scene, mutableState.render);
            adapter.SetFramesPerSecond(currentFramesPerSecond);
            adapter.SetAcrylicSettings(mutableState.ui.blurEnabled, mutableState.ui.blurStrength, mutableState.ui.darkMode);
            adapter.BeginImGuiFrame();
            MD3::BeginFrame(1.0f / 60.0f);
            MD3::SetDpiScale(1.0f);
            MD3::SetScreenSize(static_cast<float>(mutableState.window.width),
                               static_cast<float>(mutableState.window.height));
            Md3PanelCallbacks panelCallbacks{
                [&] { if (!smokeMode && !performanceSmoke && !fullscreenSmoke) settings.Save(controller.State()); },
                [&] {
                    const auto effect = controller.Dispatch(App::SetFullscreen{!controller.State().window.fullscreen});
                    if (effect.windowChanged) host.ToggleFullscreen();
                },
                [&] {
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
                    if (camera.Permission() == Services::Camera::Authorization::NotDetermined) camera.RequestPermission();
                    cameraSelector.Show();
#endif
                },
                [&] { if (ParticleSaturn::Platform::MacOS::RestartApplication()) [NSApp terminate:nil]; },
                [&](App::WindowMaterial material) { host.SetWindowMaterial(material); },
                [&](ImDrawList*, const ImVec2& position, const ImVec2& size, float) {
                    adapter.SetAcrylicPanelRect(position.x, position.y, size.x, size.y,
                                                mutableState.window.dpiScale);
                },
                {}};
            if (void* weakBlurTexture = adapter.UiWeakBlurImGuiTexture()) {
                panelCallbacks.drawGraphAcrylic = [&, weakBlurTexture](ImDrawList* drawList, const ImVec2& position,
                                                                       const ImVec2& regionSize, float rounding) {
                    const float screenWidth = std::max(1.0f, static_cast<float>(mutableState.window.width));
                    const float screenHeight = std::max(1.0f, static_cast<float>(mutableState.window.height));
                    MD3::AddImageRounded(drawList, weakBlurTexture, position,
                                         ImVec2(position.x + regionSize.x, position.y + regionSize.y),
                                         ImVec2(position.x / screenWidth, position.y / screenHeight),
                                         ImVec2((position.x + regionSize.x) / screenWidth,
                                                (position.y + regionSize.y) / screenHeight),
                                         IM_COL32_WHITE, rounding);
                };
            }
            RenderMd3Panel(controller, adapter.AdapterName().c_str(), currentFramesPerSecond, false, panelCallbacks,
                           handStatus);
            MD3::EndFrame();
            if (!adapter.PresentSceneFrame(syncInterval)) {
                if (adapter.DeviceLost()) {
                    Services::Diagnostics::DiagnosticBus::Instance().Publish(
                        "vulkan", "device-recovery", "Recreating Vulkan device after device loss",
                        Services::Diagnostics::Severity::Warning);
                    adapter.Shutdown();
                    std::string recoveryError;
                    if (adapter.Initialize(mutableState.render.vulkanDriver, resourcesPath, recoveryError) &&
                        adapter.CreateSwapChain(host.NativeView(), drawableSize.width, drawableSize.height, recoveryError) &&
                        adapter.InitializeImGui(host.NativeView(), recoveryError)) {
                        Services::Diagnostics::DiagnosticBus::Instance().Publish(
                            "vulkan", "device-recovered", "Vulkan device recreation completed",
                            Services::Diagnostics::Severity::Info);
                        return;
                    }
                    ReportStartupFailure("device-recovery", recoveryError.empty()
                        ? "Vulkan device recreation failed" : recoveryError);
                    runtimeFailed = true;
                    host.RequestExit();
                    return;
                }
                ReportStartupFailure("present", "Vulkan frame presentation failed");
                runtimeFailed = true;
                host.RequestExit();
                return;
            }
            if (captureBaseline && adapter.BaselineCaptureRequested()) host.RequestExit();
            smokeHarness.TickPerformance(controller.State());
            if (fullscreenSmoke) {
                std::int32_t x = 0;
                std::int32_t y = 0;
                host.GetWindowPosition(x, y);
                smokeHarness.TickFullscreen(nativeFullscreen, mutableState, mutableState.window.width,
                                            mutableState.window.height, x, y);
            }
            if (smokeFrames != 0 && ++renderedFrames >= smokeFrames) {
                if (lodSmoke && controller.State().render.particleCount >= App::RenderSettings::MaxParticles) {
                    ReportStartupFailure("lod-smoke", "[smoke] FAILED: Vulkan dynamic LOD smoke test did not reduce particle count");
                    runtimeFailed = true;
                }
                host.RequestExit();
            }
        });
        if (!smokeMode && !performanceSmoke && !fullscreenSmoke) settings.Save(controller.State());
        adapter.Shutdown();
        if (runtimeFailed || smokeHarness.Failed()) return 1;
    }
    return 0;
}
