#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include "CocoaHost.h"
#include "MacOSMd3Panel.h"
#include "MacOSApplication.h"
#include "MD3.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/metal/MetalBackend.h"
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/camera/macos/AVFoundationCamera.h"
#include "services/camera/macos/CameraSelectorWindow.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"
#include "services/resources/macos/BundleResources.h"
#include "services/settings/macos/NSUserDefaultsStore.h"
#include "services/diagnostics/macos/MacOSCrashHandler.h"

namespace {

class FpsMeter {
public:
    void AddSample(float deltaTime) {
        if (deltaTime <= 0.0f || deltaTime >= 1.0f) return;
        samples_[next_] = deltaTime;
        next_ = (next_ + 1U) % samples_.size();
        float total = 0.0f;
        for (const float sample : samples_) total += sample;
        framesPerSecond_ = total > 0.0f ? static_cast<float>(samples_.size()) / total : 60.0f;
    }

    std::uint32_t Value() const {
        return static_cast<std::uint32_t>(std::clamp(framesPerSecond_, 0.0f, 999.0f));
    }

private:
    std::array<float, 60> samples_{};
    std::size_t next_ = 0;
    float framesPerSecond_ = 60.0f;
};

bool WriteBaselinePpm(void* nativeDevice, void* nativeTexture, std::uint32_t width, std::uint32_t height, const char* path) {
    if (nativeDevice == nullptr || nativeTexture == nullptr || path == nullptr || path[0] == '\0' || width == 0 || height == 0) {
        return false;
    }
    id<MTLDevice> device = (id<MTLDevice>)nativeDevice;
    const NSUInteger bytesPerRow = static_cast<NSUInteger>(width) * 4U;
    id<MTLBuffer> staging = [device newBufferWithLength:bytesPerRow * height options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
    [encoder copyFromTexture:(id<MTLTexture>)nativeTexture sourceSlice:0 sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                   toBuffer:staging destinationOffset:0 destinationBytesPerRow:bytesPerRow
         destinationBytesPerImage:bytesPerRow * height];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    const bool completed = [commands status] == MTLCommandBufferStatusCompleted;
    std::ofstream output{path, std::ios::binary};
    if (completed && output) {
        output << "P6\n" << width << ' ' << height << "\n255\n";
        const auto* pixels = static_cast<const std::uint8_t*>([staging contents]);
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t column = 0; column < width; ++column) {
                const auto* pixel = &pixels[(static_cast<std::size_t>(row) * width + column) * 4U];
                const std::uint8_t rgb[] = {pixel[2], pixel[1], pixel[0]};
                output.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
            }
        }
    }
    [queue release];
    [staging release];
    return completed && output.good();
}

std::string PipelineArchivePath(id<MTLDevice> device, const char* libraryPath) {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](const std::string& value) {
        for (const unsigned char character : value) {
            hash ^= character;
            hash *= 1099511628211ULL;
        }
    };
    std::error_code error;
    const std::filesystem::path path{libraryPath};
    mix(path.string());
    mix(std::to_string(std::filesystem::file_size(path, error)));
    error.clear();
    const auto modifiedAt = std::filesystem::last_write_time(path, error).time_since_epoch().count();
    mix(std::to_string(static_cast<std::int64_t>(modifiedAt)));
    mix([[device name] UTF8String]);
    mix([[[NSProcessInfo processInfo] operatingSystemVersionString] UTF8String]);
    return (std::filesystem::temp_directory_path() /
            ("ParticleSaturn-metal-" + std::to_string(hash) + ".metallibarchive")).string();
}

} // namespace

int ParticleSaturn::Platform::MacOS::RunMetalApplication() {
    @autoreleasepool {
        ParticleSaturn::Services::Diagnostics::MacOS::InstallCrashHandler();
        ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
        const char* baselinePath = std::getenv("PARTICLESATURN_CAPTURE_BASELINE");
        const bool captureBaseline = baselinePath != nullptr && baselinePath[0] != '\0';
        auto initialState = captureBaseline ? ParticleSaturn::App::AppState{} : settings.Load({});
        if (captureBaseline) {
            initialState.window.width = 1512;
            initialState.window.height = 827;
            initialState.scene.paused = true;
            initialState.lod.locked = true;
        }
        ParticleSaturn::Platform::MacOS::CocoaHost host{initialState.window.width, initialState.window.height, "Particle Saturn"};
        if (!captureBaseline) host.SetWindowPosition(initialState.window.x, initialState.window.y);
        ParticleSaturn::Gpu::Metal::MetalDevice device;
        if (!device.Initialize()) {
            return 1;
        }
        ParticleSaturn::Gpu::Metal::MetalSurface surface{device, host.NativeMetalLayer()};
        auto size = host.CurrentDrawableSize();
        const auto libraryPath = [[[NSBundle mainBundle] pathForResource:@"ParticleKernels" ofType:@"metallib"] UTF8String];
        const auto pipelineCachePath = libraryPath == nullptr ? std::string{} :
            PipelineArchivePath((id<MTLDevice>)device.NativeDevice(), libraryPath);
        ParticleSaturn::Gpu::Metal::MetalPipelineCache pipelineCache;
        if (libraryPath != nullptr && pipelineCache.Load(device, pipelineCachePath)) {
            pipelineCache.AddComputeFunction(device, libraryPath, "InitializeParticles");
            pipelineCache.AddComputeFunction(device, libraryPath, "SimulateParticles");
            pipelineCache.AddComputeFunction(device, libraryPath, "ToneMapWithBloom");
            pipelineCache.AddComputeFunction(device, libraryPath, "BloomDownsample");
            pipelineCache.AddComputeFunction(device, libraryPath, "KawaseBlur");
            pipelineCache.AddComputeFunction(device, libraryPath, "AcrylicComposite");
            pipelineCache.AddComputeFunction(device, libraryPath, "RenderSevenSegmentFps");
            pipelineCache.AddRenderFunctions(device, libraryPath, "ParticleVertex", "ParticleFragment");
            pipelineCache.AddRenderFunctions(device, libraryPath, "StarVertex", "StarFragment");
            pipelineCache.Save(pipelineCachePath);
        }
        ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
        ParticleSaturn::Gpu::Metal::MetalStarField stars;
        ParticleSaturn::Gpu::Metal::MetalRenderTargets targets;
        if (libraryPath == nullptr || !particles.Initialize(device, libraryPath, 0x53415455U) ||
            !stars.Initialize(device, libraryPath, 0x53544152U) || !targets.Create(device, size.width, size.height)) return 1;
        ParticleSaturn::Gpu::Metal::MetalFrameRenderer renderer;
        ParticleSaturn::Gpu::Metal::MetalParticleRenderer particleRenderer;
        ParticleSaturn::App::AppController controller{initialState};
        ParticleSaturn::Services::Camera::MacOS::AVFoundationCamera camera;
        ParticleSaturn::Services::Camera::MacOS::CameraSelectorWindow cameraSelector{camera};
        if (!captureBaseline) cameraSelector.StartSaved();
        host.SetActionCallback([&](ParticleSaturn::Platform::MacOS::HostAction action) {
            switch (action) {
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleDebugWindow:
                controller.Dispatch(ParticleSaturn::App::ToggleDebugWindow{});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleFullscreen: {
                const auto effect = controller.Dispatch(ParticleSaturn::App::SetFullscreen{
                    !controller.State().window.fullscreen});
                if (effect.windowChanged) host.ToggleFullscreen();
                break;
            }
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleBlur:
                controller.Dispatch(ParticleSaturn::App::SetBlurEnabled{!controller.State().ui.blurEnabled});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::TogglePause:
                controller.Dispatch(ParticleSaturn::App::TogglePause{});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::ShowCameraSelector:
                if (camera.Permission() == ParticleSaturn::Services::Camera::Authorization::NotDetermined) camera.RequestPermission();
                cameraSelector.Show();
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::KeyF3Down:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyF3Up:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyF11Down:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyF11Up:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyBDown:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyBUp:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyEscapeDown:
            case ParticleSaturn::Platform::MacOS::HostAction::KeyEscapeUp: {
                const bool pressed = action == ParticleSaturn::Platform::MacOS::HostAction::KeyF3Down ||
                    action == ParticleSaturn::Platform::MacOS::HostAction::KeyF11Down ||
                    action == ParticleSaturn::Platform::MacOS::HostAction::KeyBDown ||
                    action == ParticleSaturn::Platform::MacOS::HostAction::KeyEscapeDown;
                const auto key = (action == ParticleSaturn::Platform::MacOS::HostAction::KeyF3Down ||
                                  action == ParticleSaturn::Platform::MacOS::HostAction::KeyF3Up)
                    ? ParticleSaturn::App::InputKey::F3
                    : (action == ParticleSaturn::Platform::MacOS::HostAction::KeyF11Down ||
                       action == ParticleSaturn::Platform::MacOS::HostAction::KeyF11Up)
                    ? ParticleSaturn::App::InputKey::F11
                    : (action == ParticleSaturn::Platform::MacOS::HostAction::KeyBDown ||
                       action == ParticleSaturn::Platform::MacOS::HostAction::KeyBUp)
                    ? ParticleSaturn::App::InputKey::B
                    : ParticleSaturn::App::InputKey::Escape;
                const auto effect = controller.Dispatch(ParticleSaturn::App::SetInputKeyPressed{key, pressed});
                if (effect.windowChanged) host.ToggleFullscreen();
                if (effect.exitRequested) host.RequestExit();
                break;
            }
            }
            if (!captureBaseline) settings.Save(controller.State());
        });
        bool baselineCaptured = false;
        std::uint32_t baselineFrameCount = 0;
        auto appliedWindowMaterial = controller.State().window.material;
        auto appliedVsyncMode = controller.State().render.vsyncMode;
        host.SetWindowMaterial(appliedWindowMaterial);
        host.SetPresentationMode(appliedVsyncMode);
        ParticleSaturn::App::FrameCoordinator coordinator;
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
        ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime handTrackingRuntime;
        std::unique_ptr<ParticleSaturn::Services::HandTracking::MacOS::HandTrackingWorker> handTracking;
        std::string handTrackingError;
        const auto palmModel = ParticleSaturn::Services::Resources::MacOS::LocateModel("palm_detection_full.tflite");
        const auto landmarkModel = ParticleSaturn::Services::Resources::MacOS::LocateModel("hand_landmark_full.tflite");
        if (!handTrackingRuntime.Load(palmModel, landmarkModel, handTrackingError)) {
            std::clog << "[HandTracking] " << handTrackingError << '\n';
        } else {
            handTracking = std::make_unique<ParticleSaturn::Services::HandTracking::MacOS::HandTrackingWorker>(
                [&handTrackingRuntime](const ParticleSaturn::Services::Camera::Frame& frame,
                                       ParticleSaturn::Services::HandTracking::MacOS::HandPose& pose,
                                       std::string& error) {
                    return handTrackingRuntime.Invoke(frame, error) && handTrackingRuntime.DecodeLandmarks(pose);
                });
            handTracking->Start();
        }
#endif
        FpsMeter fpsMeter;
        auto lastFrameTime = std::chrono::steady_clock::now();
        if (!particleRenderer.Initialize(device, libraryPath)) return 1;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().FontDefault = ImGui::GetIO().Fonts->AddFontFromFileTTF("/System/Library/Fonts/SFNS.ttf", 15.0f);
        ImGui::GetStyle().WindowRounding = 12.0f;
        ImGui::GetStyle().WindowBorderSize = 0.0f;
        MD3::Init();
        MD3::SetDarkMode(controller.State().ui.darkMode);
        MD3::SetScreenSize(static_cast<float>(size.width / size.scale), static_cast<float>(size.height / size.scale));
        if (!ImGui_ImplOSX_Init((NSView*)host.NativeView()) || !ImGui_ImplMetal_Init((id<MTLDevice>)device.NativeDevice())) return 1;
        host.Show();
        host.Run([&] {
            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::clamp(std::chrono::duration<float>(now - lastFrameTime).count(), 0.0f, 0.25f);
            lastFrameTime = now;
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
            ParticleSaturn::App::GestureInput gesture;
            ParticleSaturn::Services::Camera::Frame cameraFrame;
            if (handTracking && camera.LatestFrame(cameraFrame)) {
                handTracking->Submit(std::move(cameraFrame), controller.State().gesture.handLostDelay);
            }
            if (handTracking) gesture = handTracking->LatestGesture();
            const bool handTracked = gesture.tracked;
            const auto frame = coordinator.Advance(controller, deltaTime, gesture);
#else
            const bool handTracked = false;
            const auto frame = coordinator.Advance(controller, deltaTime);
#endif
            fpsMeter.AddSample(deltaTime);
            if (frame.state->window.material != appliedWindowMaterial) {
                host.SetWindowMaterial(frame.state->window.material);
                appliedWindowMaterial = frame.state->window.material;
            }
            if (frame.state->render.vsyncMode != appliedVsyncMode) {
                host.SetPresentationMode(frame.state->render.vsyncMode);
                appliedVsyncMode = frame.state->render.vsyncMode;
            }
            const auto drawableSize = host.CurrentDrawableSize();
            auto& mutableState = controller.MutableState();
            mutableState.window.width = static_cast<std::uint32_t>(drawableSize.width / drawableSize.scale);
            mutableState.window.height = static_cast<std::uint32_t>(drawableSize.height / drawableSize.scale);
            mutableState.window.dpiScale = drawableSize.scale;
            if (!mutableState.window.fullscreen) {
                host.GetWindowPosition(mutableState.window.x, mutableState.window.y);
                mutableState.window.windowedX = mutableState.window.x;
                mutableState.window.windowedY = mutableState.window.y;
                mutableState.window.windowedWidth = mutableState.window.width;
                mutableState.window.windowedHeight = mutableState.window.height;
            }
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath, drawableSize.width, drawableSize.height,
                            drawableSize.scale, *frame.state, handTracked, deltaTime, fpsMeter.Value(), [&](void* commands, void* encoder, void* pass, void* uiOverlayTexture) {
                ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)pass);
                ImGui_ImplOSX_NewFrame((NSView*)host.NativeView());
                ImGui::NewFrame();
                MD3::BeginFrame(deltaTime);
                MD3::SetDpiScale(1.0f);
                MD3::SetScreenSize(static_cast<float>(drawableSize.width / drawableSize.scale),
                                   static_cast<float>(drawableSize.height / drawableSize.scale));
                ParticleSaturn::Platform::MacOS::RenderMd3Panel(controller, "Metal", fpsMeter.Value(), false, {
                    [&] { if (!captureBaseline) settings.Save(controller.State()); },
                    [&] {
                        const auto effect = controller.Dispatch(ParticleSaturn::App::SetFullscreen{!controller.State().window.fullscreen});
                        if (effect.windowChanged) host.ToggleFullscreen();
                    },
                    [&] {
                        if (camera.Permission() == ParticleSaturn::Services::Camera::Authorization::NotDetermined) camera.RequestPermission();
                        cameraSelector.Show();
                    },
                    [&] { if (ParticleSaturn::Platform::MacOS::RestartApplication()) [NSApp terminate:nil]; },
                    [&](ParticleSaturn::App::WindowMaterial material) { host.SetWindowMaterial(material); },
                    [&](ImDrawList* drawList, const ImVec2& position, const ImVec2& panelSize) {
                        const float left = position.x * drawableSize.scale / static_cast<float>(drawableSize.width);
                        const float top = position.y * drawableSize.scale / static_cast<float>(drawableSize.height);
                        const float right = (position.x + panelSize.x) * drawableSize.scale / static_cast<float>(drawableSize.width);
                        const float bottom = (position.y + panelSize.y) * drawableSize.scale / static_cast<float>(drawableSize.height);
                        MD3::AddImageRounded(drawList, uiOverlayTexture, position,
                                             ImVec2(position.x + panelSize.x, position.y + panelSize.y),
                                             ImVec2(left, top), ImVec2(right, bottom), IM_COL32_WHITE, 12.0f);
                    }});
                MD3::EndFrame();
                ImGui::Render();
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), (id<MTLCommandBuffer>)commands,
                                               (id<MTLRenderCommandEncoder>)encoder);
            }, [&](void* nativeDevice, void* texture, std::uint32_t width, std::uint32_t height) {
                if (!captureBaseline || baselineCaptured) return true;
                if (++baselineFrameCount < 3U) return true;
                baselineCaptured = WriteBaselinePpm(nativeDevice, texture, width, height, baselinePath);
                if (baselineCaptured) [NSApp terminate:nil];
                return baselineCaptured;
            });
        });
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        MD3::Shutdown();
        ImGui::DestroyContext();
        if (!captureBaseline) settings.Save(controller.State());
    }
    return 0;
}
