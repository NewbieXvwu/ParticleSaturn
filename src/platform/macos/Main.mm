#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
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
#include "AppShell.h"
#include "SmokeHarness.h"
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
        InstallDebugLogCapture();
        ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
        const auto smoke = ParticleSaturn::Platform::MacOS::SmokeConfig::FromEnvironment();
        const bool captureBaseline = smoke.captureBaseline;
        auto initialState = smoke.Deterministic() ? ParticleSaturn::App::AppState{} : settings.Load({});
        smoke.ForceInitialState(initialState);
        const auto startup = ParticleSaturn::Platform::MacOS::ResolveStartupGeometry(initialState);
        const auto startupWidth = startup.width;
        const auto startupHeight = startup.height;
        ParticleSaturn::Platform::MacOS::CocoaHost host{startupWidth, startupHeight, "Particle Saturn"};
        if (!captureBaseline) host.SetWindowPosition(startup.x, startup.y);
        ParticleSaturn::Platform::MacOS::SmokeHarness smokeHarness{smoke, startup, "metal", {
            [&host] { [[(NSView*)host.NativeView() window] toggleFullScreen:nil]; },
            [&host] { host.RequestExit(); }}};
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
        bool baselineCaptured = false;
        std::uint32_t baselineFrameCount = 0;
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
        ParticleSaturn::Platform::MacOS::CocoaAppHost appHost{host};
        ParticleSaturn::Platform::MacOS::BackendCapabilities capabilities;
        capabilities.objectShaderParticles = particleRenderer.ObjectShaderAvailable();
        if (capabilities.objectShaderParticles) {
            capabilities.declaredDivergences.push_back(
                "Metal object/mesh shader 粒子路径保持手写 MSL（D-004 声明分歧）");
        }
        ParticleSaturn::Platform::MacOS::RunAppConfig shellConfig{
            appHost, controller, settings, smoke, smokeHarness, startup,
            "Metal", capabilities,
            /*persistSettings=*/!smoke.Deterministic(),
            /*cameraEnabled=*/!smoke.Deterministic(),
            /*fixedDeltaTime=*/0.0f, {}, {}};
        shellConfig.renderFrame = [&](const ParticleSaturn::Platform::MacOS::FrameContext& frame) {
            const auto& drawableSize = frame.drawableSize;
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath,
                            drawableSize.width, drawableSize.height, drawableSize.scale, frame.state,
                            frame.handTracked, frame.deltaTime, frame.framesPerSecond,
                            [&](void* commands, void* encoder, void* pass, void* uiOverlayTexture) {
                ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)pass);
                ImGui_ImplOSX_NewFrame((NSView*)host.NativeView());
                ImGui::NewFrame();
                MD3::BeginFrame(frame.deltaTime);
                MD3::SetDpiScale(1.0f);
                MD3::SetScreenSize(static_cast<float>(drawableSize.width / drawableSize.scale),
                                   static_cast<float>(drawableSize.height / drawableSize.scale));
                ParticleSaturn::Platform::MacOS::BackendPanelHooks hooks;
                hooks.drawAcrylicBackground = [&](ImDrawList* drawList, const ImVec2& position,
                                                  const ImVec2& panelSize, float rounding) {
                    const float left = position.x * drawableSize.scale / static_cast<float>(drawableSize.width);
                    const float top = position.y * drawableSize.scale / static_cast<float>(drawableSize.height);
                    const float right = (position.x + panelSize.x) * drawableSize.scale / static_cast<float>(drawableSize.width);
                    const float bottom = (position.y + panelSize.y) * drawableSize.scale / static_cast<float>(drawableSize.height);
                    MD3::AddImageRounded(drawList, uiOverlayTexture, position,
                                         ImVec2(position.x + panelSize.x, position.y + panelSize.y),
                                         ImVec2(left, top), ImVec2(right, bottom), IM_COL32_WHITE, rounding);
                };
                hooks.drawGraphAcrylic = [&](ImDrawList* drawList, const ImVec2& position,
                                             const ImVec2& regionSize, float rounding) {
                    const float left = position.x * drawableSize.scale / static_cast<float>(drawableSize.width);
                    const float top = position.y * drawableSize.scale / static_cast<float>(drawableSize.height);
                    const float right = (position.x + regionSize.x) * drawableSize.scale / static_cast<float>(drawableSize.width);
                    const float bottom = (position.y + regionSize.y) * drawableSize.scale / static_cast<float>(drawableSize.height);
                    MD3::AddImageRounded(drawList, uiOverlayTexture, position,
                                         ImVec2(position.x + regionSize.x, position.y + regionSize.y),
                                         ImVec2(left, top), ImVec2(right, bottom), IM_COL32_WHITE, rounding);
                };
                frame.drawPanel(hooks);
                MD3::EndFrame();
                ImGui::Render();
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), (id<MTLCommandBuffer>)commands,
                                               (id<MTLRenderCommandEncoder>)encoder);
            }, [&](void* nativeDevice, void* texture, std::uint32_t width, std::uint32_t height) {
                if (!smoke.captureBaseline || baselineCaptured) return true;
                if (++baselineFrameCount < 3U) return true;
                baselineCaptured = WriteBaselinePpm(nativeDevice, texture, width, height, smoke.baselinePath);
                if (baselineCaptured) host.RequestExit();
                return baselineCaptured;
            });
            return true;
        };
        const int exitCode = ParticleSaturn::Platform::MacOS::RunApp(shellConfig);
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        MD3::Shutdown();
        ImGui::DestroyContext();
        return exitCode;
    }
}
