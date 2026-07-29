#import <Cocoa/Cocoa.h>

#include "AppShell.h"

#include "imgui.h"

#include "MacOSApplication.h"
#include "MD3.h"
#include "app/FpsMeter.h"
#include "services/diagnostics/DiagnosticBus.h"
#include "services/camera/macos/AVFoundationCamera.h"
#include "services/camera/macos/CameraSelectorWindow.h"
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"
#include "services/resources/macos/BundleResources.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <utility>

namespace ParticleSaturn::Platform::MacOS {

bool CocoaAppHost::NativeFullscreen() {
    return ([[(NSView*)host_.NativeView() window] styleMask] & NSWindowStyleMaskFullScreen) != 0;
}

int RunApp(RunAppConfig& config) {
    auto& host = config.host;
    auto& controller = config.controller;
    auto& backend = *config.backend;

    // 声明分歧登记（D-004）：故意的实验变量在诊断总线留档，与意外漂移区分。
    for (const auto& divergence : backend.Capabilities().declaredDivergences) {
        Services::Diagnostics::DiagnosticBus::Instance().Publish(
            "backend", "declared-divergence", divergence, Services::Diagnostics::Severity::Info);
    }

    Services::Camera::MacOS::AVFoundationCamera camera;
    Services::Camera::MacOS::CameraSelectorWindow cameraSelector{camera};
    if (config.cameraEnabled) cameraSelector.StartSaved();

#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
    Services::HandTracking::MacOS::XnnpackHandTrackingRuntime handTrackingRuntime;
    std::unique_ptr<Services::HandTracking::MacOS::HandTrackingWorker> handTracking;
    std::string handTrackingError;
    std::uint32_t lastCameraFrameWidth = 0;
    std::uint32_t lastCameraFrameHeight = 0;
    if (config.cameraEnabled) {
        const auto palmModel = Services::Resources::MacOS::LocateModel("palm_detection_full.tflite");
        const auto landmarkModel = Services::Resources::MacOS::LocateModel("hand_landmark_full.tflite");
        if (!handTrackingRuntime.Load(palmModel, landmarkModel, handTrackingError)) {
            std::clog << "[HandTracking] " << handTrackingError << '\n';
        } else {
            handTracking = std::make_unique<Services::HandTracking::MacOS::HandTrackingWorker>(
                [&handTrackingRuntime](const Services::Camera::Frame& frame,
                                       Services::HandTracking::MacOS::HandPose& pose, std::string& error) {
                    return handTrackingRuntime.Invoke(frame, error) && handTrackingRuntime.DecodeLandmarks(pose);
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
        case HostAction::ToggleFullscreen: {
            const auto effect = controller.Dispatch(App::SetFullscreen{!controller.State().window.fullscreen});
            if (effect.windowChanged) host.ToggleFullscreen();
            break;
        }
        case HostAction::ToggleBlur:
            controller.Dispatch(App::SetBlurEnabled{!controller.State().ui.blurEnabled});
            break;
        case HostAction::TogglePause:
            controller.Dispatch(App::TogglePause{});
            break;
        case HostAction::NativeFullscreenEntered:
            controller.Dispatch(App::SetFullscreen{true});
            break;
        case HostAction::NativeFullscreenExited:
            controller.Dispatch(App::SetFullscreen{false});
            break;
        case HostAction::ShowCameraSelector:
            if (config.cameraEnabled) {
                if (camera.Permission() == Services::Camera::Authorization::NotDetermined) camera.RequestPermission();
                cameraSelector.Show();
            }
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
        }
        if (config.persistSettings) config.settings.Save(controller.State());
    });

    if (config.preRun) {
        const int preRunExit = config.preRun();
        if (preRunExit != 0) return preRunExit;
    }

    auto appliedWindowMaterial = controller.State().window.material;
    auto appliedVsyncMode = controller.State().render.vsyncMode;
    host.SetWindowMaterial(appliedWindowMaterial);
    host.SetPresentationMode(appliedVsyncMode);

    App::FrameCoordinator coordinator;
    App::FpsMeter fpsMeter;
    auto lastFrameTime = std::chrono::steady_clock::now();

    host.Show();
    if (config.startup.restoreFullscreen) host.ToggleFullscreen();
    host.Run([&] {
        const auto now = std::chrono::steady_clock::now();
        float deltaTime = std::clamp(std::chrono::duration<float>(now - lastFrameTime).count(), 0.0f, 0.25f);
        lastFrameTime = now;
        if (config.fixedDeltaTime > 0.0f) deltaTime = config.fixedDeltaTime;

        App::GestureInput gesture;
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
        Services::Camera::Frame cameraFrame;
        if (handTracking && camera.LatestFrame(cameraFrame)) {
            lastCameraFrameWidth = cameraFrame.width;
            lastCameraFrameHeight = cameraFrame.height;
            handTracking->Submit(std::move(cameraFrame), controller.State().gesture.handLostDelay);
        }
        if (handTracking) gesture = handTracking->LatestGesture();
        const bool handTracked = gesture.tracked;
        coordinator.Advance(controller, deltaTime, gesture);
#else
        const bool handTracked = false;
        coordinator.Advance(controller, deltaTime);
#endif
        fpsMeter.AddSample(deltaTime);

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
                std::snprintf(cameraInfo, sizeof(cameraInfo), "%u x %u", lastCameraFrameWidth, lastCameraFrameHeight);
                handStatus.cameraInfo = cameraInfo;
            }
            handStatus.handDetected = gesture.tracked;
            handStatus.rawScale = gesture.scale;
            handStatus.rawRotX = gesture.rotationXNormalized;
            handStatus.rawRotY = gesture.rotationYNormalized;
        }
#endif

        auto& mutableState = controller.MutableState();
        if (mutableState.window.material != appliedWindowMaterial) {
            host.SetWindowMaterial(mutableState.window.material);
            appliedWindowMaterial = mutableState.window.material;
        }
        if (mutableState.render.vsyncMode != appliedVsyncMode) {
            host.SetPresentationMode(mutableState.render.vsyncMode);
            appliedVsyncMode = mutableState.render.vsyncMode;
        }
        const auto drawableSize = host.CurrentDrawableSize();
        mutableState.window.width = static_cast<std::uint32_t>(drawableSize.width / drawableSize.scale);
        mutableState.window.height = static_cast<std::uint32_t>(drawableSize.height / drawableSize.scale);
        mutableState.window.dpiScale = drawableSize.scale;
        const bool nativeFullscreen = host.NativeFullscreen();
        if (!nativeFullscreen) {
            host.WindowPosition(mutableState.window.x, mutableState.window.y);
            mutableState.window.windowedX = mutableState.window.x;
            mutableState.window.windowedY = mutableState.window.y;
            mutableState.window.windowedWidth = mutableState.window.width;
            mutableState.window.windowedHeight = mutableState.window.height;
        }

        const std::function<void(const BackendPanelHooks&)> drawPanel = [&](const BackendPanelHooks& hooks) {
            const Md3PanelCallbacks callbacks{
                [&] { if (config.persistSettings) config.settings.Save(controller.State()); },
                [&] {
                    const auto effect =
                        controller.Dispatch(App::SetFullscreen{!controller.State().window.fullscreen});
                    if (effect.windowChanged) host.ToggleFullscreen();
                },
                [&] {
                    if (!config.cameraEnabled) return;
                    if (camera.Permission() == Services::Camera::Authorization::NotDetermined)
                        camera.RequestPermission();
                    cameraSelector.Show();
                },
                [] { if (RestartApplication()) [NSApp terminate:nil]; },
                [&](App::WindowMaterial material) { host.SetWindowMaterial(material); },
                hooks.drawAcrylicBackground,
                hooks.drawGraphAcrylic};
            const Md3PanelBackendFeatures features{backend.Capabilities().analyticParticles,
                                                   backend.Capabilities().objectShaderParticles};
            RenderMd3Panel(controller, config.panelTitle.c_str(), fpsMeter.Value(), features, callbacks, handStatus);
        };

        const FrameContext context{mutableState, deltaTime, handTracked, gesture,
                                   fpsMeter.Value(), drawableSize, nativeFullscreen, drawPanel};
        const bool frameCompleted = backend.RenderFrame(context);
        // P4 读回收束（原先三个 main 各自重复）：基线一落盘即请求退出。捕获帧
        // 本身可能中止呈现（GL41 捕获后不再 swap），故不看 frameCompleted。
        if (config.smoke.captureBaseline && backend.BaselineCaptured()) {
            host.RequestExit();
            return;
        }
        if (!frameCompleted) return;

        config.smokeHarness.TickPerformance(controller.State());
        if (config.smoke.fullscreenSmoke) {
            std::int32_t x = 0;
            std::int32_t y = 0;
            host.WindowPosition(x, y);
            config.smokeHarness.TickFullscreen(nativeFullscreen, mutableState, mutableState.window.width,
                                               mutableState.window.height, x, y);
        }
    });

    if (config.persistSettings) config.settings.Save(controller.State());
    return config.smokeHarness.Failed() ? 1 : 0;
}

} // namespace ParticleSaturn::Platform::MacOS
