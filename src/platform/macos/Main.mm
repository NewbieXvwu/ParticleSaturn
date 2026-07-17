#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
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
#include "MacOSApplication.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/metal/MetalBackend.h"
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/camera/macos/AVFoundationCamera.h"
#include "services/camera/macos/CameraSelectorWindow.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"
#include "services/resources/macos/BundleResources.h"
#include "services/settings/macos/NSUserDefaultsStore.h"

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

} // namespace

int ParticleSaturn::Platform::MacOS::RunMetalApplication() {
    @autoreleasepool {
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
        const auto pipelineCachePath = (std::filesystem::temp_directory_path() / "ParticleSaturn-v2.metallibarchive").string();
        ParticleSaturn::Gpu::Metal::MetalPipelineCache pipelineCache;
        if (libraryPath != nullptr && pipelineCache.Load(device, pipelineCachePath)) {
            pipelineCache.AddComputeFunction(device, libraryPath, "InitializeParticles");
            pipelineCache.AddComputeFunction(device, libraryPath, "SimulateParticles");
            pipelineCache.AddComputeFunction(device, libraryPath, "ToneMapWithBloom");
            pipelineCache.AddComputeFunction(device, libraryPath, "BloomDownsample");
            pipelineCache.AddComputeFunction(device, libraryPath, "KawaseBlur");
            pipelineCache.AddComputeFunction(device, libraryPath, "AcrylicComposite");
            pipelineCache.AddComputeFunction(device, libraryPath, "RenderSevenSegmentFps");
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
        ParticleSaturn::Services::Camera::MacOS::AVFoundationCamera camera;
        ParticleSaturn::Services::Camera::MacOS::CameraSelectorWindow cameraSelector{camera};
        if (!captureBaseline) cameraSelector.StartSaved();
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
        ImGui::StyleColorsDark();
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
            if (drawableSize.width != size.width || drawableSize.height != size.height) {
                if (!targets.Create(device, drawableSize.width, drawableSize.height, &renderer.Scheduler())) return;
                size = drawableSize;
            }
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath, drawableSize.width, drawableSize.height,
                            drawableSize.scale, *frame.state, handTracked, deltaTime, fpsMeter.Value(), [&](void* commands, void* encoder, void* pass) {
                ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)pass);
                ImGui_ImplOSX_NewFrame((NSView*)host.NativeView());
                ImGui::NewFrame();
                const auto& state = controller.State();
                if (state.ui.showDebugWindow) {
                    ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_Always);
                    ImGui::SetNextWindowSize(ImVec2(320.0f, 340.0f), ImGuiCond_Always);
                    ImGui::SetNextWindowBgAlpha(0.0f);
                    ImGui::Begin("Particle Saturn");
                const ImVec2 panelPosition = ImGui::GetWindowPos();
                const ImVec2 panelSize = ImGui::GetWindowSize();
                const float panelLeft = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelTop = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.height);
                const float panelRight = (80.0f + 320.0f) * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelBottom = (80.0f + 340.0f) * drawableSize.scale / static_cast<float>(drawableSize.height);
                if (state.ui.blurEnabled) {
                    ImGui::GetWindowDrawList()->AddImage((ImTextureID)targets.UiOverlay(), panelPosition,
                                                          ImVec2(panelPosition.x + panelSize.x, panelPosition.y + panelSize.y),
                                                          ImVec2(panelLeft, panelTop), ImVec2(panelRight, panelBottom));
                }
                ImGui::Text("Metal");
                ImGui::SameLine();
                ImGui::Text("FPS: %u", fpsMeter.Value());
                ImGui::Separator();
                ImGui::Text("Particles: %u", state.render.particleCount);
                int particleCount = static_cast<int>(state.render.particleCount);
                if (ImGui::SliderInt("Particle count", &particleCount,
                                     static_cast<int>(ParticleSaturn::App::RenderSettings::MinParticles),
                                     static_cast<int>(ParticleSaturn::App::RenderSettings::MaxParticles))) {
                    controller.Dispatch(ParticleSaturn::App::SetParticleCount{static_cast<std::uint32_t>(particleCount)});
                    settings.Save(controller.State());
                }
                bool bloomEnabled = state.render.bloomEnabled;
                if (ImGui::Checkbox("Bloom", &bloomEnabled)) {
                    controller.Dispatch(ParticleSaturn::App::SetBloomEnabled{bloomEnabled});
                    settings.Save(controller.State());
                }
                bool blurEnabled = state.ui.blurEnabled;
                if (ImGui::Checkbox("UI blur", &blurEnabled)) {
                    controller.Dispatch(ParticleSaturn::App::SetBlurEnabled{blurEnabled});
                    settings.Save(controller.State());
                }
                int windowMaterial = static_cast<int>(state.window.material);
                if (ImGui::Combo("Window material", &windowMaterial,
                                 "Solid\0Transparent\0System blur\0App Acrylic\0")) {
                    const auto material = static_cast<ParticleSaturn::App::WindowMaterial>(windowMaterial);
                    controller.Dispatch(ParticleSaturn::App::SetWindowMaterial{material});
                    settings.Save(controller.State());
                }
                float blurStrength = state.ui.blurStrength;
                if (ImGui::SliderFloat("Blur strength", &blurStrength, 0.0f, 5.0f)) {
                    controller.Dispatch(ParticleSaturn::App::SetBlurStrength{blurStrength});
                    settings.Save(controller.State());
                }
                int graphicsApi = static_cast<int>(state.render.graphicsApi);
                if (ImGui::Combo("Graphics API", &graphicsApi, "OpenGL 4.1\0Vulkan\0Metal\0")) {
                    const auto effect = controller.Dispatch(ParticleSaturn::App::SetGraphicsApi{
                        static_cast<ParticleSaturn::App::GraphicsApi>(graphicsApi)});
                    settings.Save(controller.State());
                    if (effect.restartRequired && ParticleSaturn::Platform::MacOS::RestartApplication()) {
                        [NSApp terminate:nil];
                        return;
                    }
                }
                if (controller.State().render.graphicsApi == ParticleSaturn::App::GraphicsApi::Vulkan) {
                    int vulkanDriver = static_cast<int>(state.render.vulkanDriver);
                    if (ImGui::Combo("Vulkan driver", &vulkanDriver, "MoltenVK\0KosmicKrisp\0")) {
                        const auto effect = controller.Dispatch(ParticleSaturn::App::SetVulkanDriver{
                            static_cast<ParticleSaturn::App::VulkanDriver>(vulkanDriver)});
                        settings.Save(controller.State());
                        if (effect.restartRequired && ParticleSaturn::Platform::MacOS::RestartApplication()) {
                            [NSApp terminate:nil];
                            return;
                        }
                    }
                }
                bool lodLocked = state.lod.locked;
                if (ImGui::Checkbox("Lock dynamic LOD", &lodLocked)) {
                    controller.Dispatch(ParticleSaturn::App::SetLodLocked{lodLocked});
                    settings.Save(controller.State());
                }
                const char* pauseLabel = state.scene.paused ? "Resume" : "Pause";
                if (ImGui::Button(pauseLabel)) {
                    controller.Dispatch(ParticleSaturn::App::TogglePause{});
                    settings.Save(controller.State());
                }
                ImGui::SameLine();
                if (ImGui::Button("Fullscreen")) {
                    const bool fullscreen = !state.window.fullscreen;
                    const auto effect = controller.Dispatch(ParticleSaturn::App::SetFullscreen{fullscreen});
                    settings.Save(controller.State());
                    if (effect.windowChanged) host.ToggleFullscreen();
                }
                ImGui::SameLine();
                if (ImGui::Button("Camera")) {
                    if (camera.Permission() == ParticleSaturn::Services::Camera::Authorization::NotDetermined) {
                        camera.RequestPermission();
                    }
                    cameraSelector.Show();
                }
                    ImGui::End();
                }
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
        ImGui::DestroyContext();
        if (!captureBaseline) settings.Save(controller.State());
    }
    return 0;
}
