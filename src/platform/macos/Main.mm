#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include "CocoaHost.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/metal/MetalBackend.h"
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

#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
class HandTrackingWorker {
public:
    ~HandTrackingWorker() { Stop(); }

    bool Start(const std::string& palmModelPath, const std::string& landmarkModelPath, std::string& error) {
        if (!runtime_.Load(palmModelPath, landmarkModelPath, error)) return false;
        worker_ = std::thread([this] { Run(); });
        return true;
    }

    void Submit(ParticleSaturn::Services::Camera::Frame frame, int handLostDelay) {
        std::lock_guard lock{inputMutex_};
        pendingFrame_ = std::move(frame);
        pendingLostDelay_ = std::clamp(handLostDelay, 1, 120);
        hasPendingFrame_ = true;
        inputReady_.notify_one();
    }

    ParticleSaturn::App::GestureInput LatestGesture() const {
        std::lock_guard lock{outputMutex_};
        ParticleSaturn::App::GestureInput gesture;
        if (!sample_.tracked || std::chrono::steady_clock::now() - sample_.updatedAt > std::chrono::milliseconds{500}) {
            return gesture;
        }
        gesture.tracked = true;
        gesture.hasAbsolutePose = true;
        gesture.rotationXNormalized = sample_.pose.centerX;
        gesture.rotationYNormalized = sample_.pose.centerY;
        gesture.scale = sample_.pose.scale;
        return gesture;
    }

private:
    struct Sample {
        bool tracked = false;
        ParticleSaturn::Services::HandTracking::MacOS::HandPose pose;
        std::chrono::steady_clock::time_point updatedAt{};
    };

    void Stop() {
        {
            std::lock_guard lock{inputMutex_};
            stopping_ = true;
            inputReady_.notify_one();
        }
        if (worker_.joinable()) worker_.join();
    }

    void Run() {
        int lostFrames = 0;
        for (;;) {
            ParticleSaturn::Services::Camera::Frame frame;
            int handLostDelay = 1;
            {
                std::unique_lock lock{inputMutex_};
                inputReady_.wait(lock, [this] { return stopping_ || hasPendingFrame_; });
                if (stopping_) return;
                frame = std::move(pendingFrame_);
                handLostDelay = pendingLostDelay_;
                hasPendingFrame_ = false;
            }
            std::string error;
            ParticleSaturn::Services::HandTracking::MacOS::HandPose pose;
            const bool tracked = runtime_.Invoke(frame, error) && runtime_.DecodeLandmarks(pose);
            std::lock_guard lock{outputMutex_};
            sample_.updatedAt = std::chrono::steady_clock::now();
            if (tracked) {
                lostFrames = 0;
                sample_.tracked = true;
                sample_.pose = pose;
            } else if (++lostFrames >= handLostDelay) {
                sample_.tracked = false;
            }
        }
    }

    ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime runtime_;
    mutable std::mutex inputMutex_;
    mutable std::mutex outputMutex_;
    std::condition_variable inputReady_;
    std::thread worker_;
    ParticleSaturn::Services::Camera::Frame pendingFrame_;
    Sample sample_;
    int pendingLostDelay_ = 1;
    bool hasPendingFrame_ = false;
    bool stopping_ = false;
};
#endif

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

int main() {
    @autoreleasepool {
        ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
        const char* baselinePath = std::getenv("PARTICLESATURN_CAPTURE_BASELINE");
        const bool captureBaseline = baselinePath != nullptr && baselinePath[0] != '\0';
        auto initialState = captureBaseline ? ParticleSaturn::App::AppState{} : settings.Load({});
        if (captureBaseline) {
            initialState.window.width = 1512;
            initialState.window.height = 827;
            initialState.scene.paused = true;
        }
        initialState.render.graphicsApi = ParticleSaturn::App::GraphicsApi::Metal;
        ParticleSaturn::Platform::MacOS::CocoaHost host{initialState.window.width, initialState.window.height, "Particle Saturn"};
        ParticleSaturn::Gpu::Metal::MetalDevice device;
        if (!device.Initialize()) {
            return 1;
        }
        ParticleSaturn::Gpu::Metal::MetalSurface surface{device, host.NativeMetalLayer()};
        auto size = host.CurrentDrawableSize();
        const auto libraryPath = [[[NSBundle mainBundle] pathForResource:@"ParticleKernels" ofType:@"metallib"] UTF8String];
        ParticleSaturn::Gpu::Metal::MetalParticleSystem particles;
        ParticleSaturn::Gpu::Metal::MetalStarField stars;
        ParticleSaturn::Gpu::Metal::MetalRenderTargets targets;
        if (libraryPath == nullptr || !particles.Initialize(device, libraryPath, 0x53415455U) ||
            !stars.Initialize(device, libraryPath, 0x53544152U) || !targets.Create(device, size.width, size.height)) return 1;
        ParticleSaturn::Gpu::Metal::MetalFrameRenderer renderer;
        ParticleSaturn::Gpu::Metal::MetalParticleRenderer particleRenderer;
        ParticleSaturn::App::AppController controller{initialState};
        bool baselineCaptured = false;
        auto appliedWindowMaterial = controller.State().window.material;
        host.SetWindowMaterial(appliedWindowMaterial);
        ParticleSaturn::App::FrameCoordinator coordinator;
        ParticleSaturn::Services::Camera::MacOS::AVFoundationCamera camera;
        ParticleSaturn::Services::Camera::MacOS::CameraSelectorWindow cameraSelector{camera};
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
        HandTrackingWorker handTracking;
        std::string handTrackingError;
        const auto palmModel = ParticleSaturn::Services::Resources::MacOS::LocateModel("palm_detection_full.tflite");
        const auto landmarkModel = ParticleSaturn::Services::Resources::MacOS::LocateModel("hand_landmark_full.tflite");
        if (!handTracking.Start(palmModel, landmarkModel, handTrackingError)) {
            std::clog << "[HandTracking] " << handTrackingError << '\n';
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
            if (camera.LatestFrame(cameraFrame)) {
                handTracking.Submit(std::move(cameraFrame), controller.State().gesture.handLostDelay);
            }
            gesture = handTracking.LatestGesture();
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
            const auto drawableSize = host.CurrentDrawableSize();
            auto& mutableState = controller.MutableState();
            mutableState.window.width = static_cast<std::uint32_t>(drawableSize.width / drawableSize.scale);
            mutableState.window.height = static_cast<std::uint32_t>(drawableSize.height / drawableSize.scale);
            mutableState.window.dpiScale = drawableSize.scale;
            if (drawableSize.width != size.width || drawableSize.height != size.height) {
                if (!targets.Create(device, drawableSize.width, drawableSize.height)) return;
                size = drawableSize;
            }
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath, drawableSize.width, drawableSize.height,
                            drawableSize.scale, *frame.state, handTracked, deltaTime, fpsMeter.Value(), [&](void* commands, void* encoder, void* pass) {
                ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)pass);
                ImGui_ImplOSX_NewFrame((NSView*)host.NativeView());
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(320.0f, 280.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.0f);
                ImGui::Begin("Particle Saturn");
                const auto& state = controller.State();
                const ImVec2 panelPosition = ImGui::GetWindowPos();
                const ImVec2 panelSize = ImGui::GetWindowSize();
                const float panelLeft = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelTop = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.height);
                const float panelRight = (80.0f + 320.0f) * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelBottom = (80.0f + 280.0f) * drawableSize.scale / static_cast<float>(drawableSize.height);
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
                bool glassEnabled = state.window.material == ParticleSaturn::App::WindowMaterial::SystemBlur;
                if (ImGui::Checkbox("Window glass", &glassEnabled)) {
                    const auto material = glassEnabled ? ParticleSaturn::App::WindowMaterial::SystemBlur
                                                       : ParticleSaturn::App::WindowMaterial::Solid;
                    controller.Dispatch(ParticleSaturn::App::SetWindowMaterial{material});
                    settings.Save(controller.State());
                }
                float blurStrength = state.ui.blurStrength;
                if (ImGui::SliderFloat("Blur strength", &blurStrength, 0.0f, 5.0f)) {
                    controller.Dispatch(ParticleSaturn::App::SetBlurStrength{blurStrength});
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
                ImGui::Render();
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), (id<MTLCommandBuffer>)commands,
                                               (id<MTLRenderCommandEncoder>)encoder);
            }, [&](void* nativeDevice, void* texture, std::uint32_t width, std::uint32_t height) {
                if (!captureBaseline || baselineCaptured) return true;
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
