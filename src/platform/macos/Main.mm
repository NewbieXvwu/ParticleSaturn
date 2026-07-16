#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include "CocoaHost.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/metal/MetalBackend.h"

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

} // namespace

int main() {
    @autoreleasepool {
        ParticleSaturn::Platform::MacOS::CocoaHost host{1280, 720, "Particle Saturn"};
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
        ParticleSaturn::App::AppController controller;
        ParticleSaturn::App::FrameCoordinator coordinator;
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
            const auto frame = coordinator.Advance(controller, deltaTime);
            fpsMeter.AddSample(deltaTime);
            const auto drawableSize = host.CurrentDrawableSize();
            if (drawableSize.width != size.width || drawableSize.height != size.height) {
                if (!targets.Create(device, drawableSize.width, drawableSize.height)) return;
                size = drawableSize;
            }
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath, drawableSize.width, drawableSize.height,
                            drawableSize.scale, frame.state->scene, false, deltaTime, fpsMeter.Value(), [&](void* commands, void* encoder, void* pass) {
                ImGui_ImplMetal_NewFrame((MTLRenderPassDescriptor*)pass);
                ImGui_ImplOSX_NewFrame((NSView*)host.NativeView());
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(210.0f, 95.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.0f);
                ImGui::Begin("Particle Saturn");
                const ImVec2 panelPosition = ImGui::GetWindowPos();
                const ImVec2 panelSize = ImGui::GetWindowSize();
                const float panelLeft = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelTop = 80.0f * drawableSize.scale / static_cast<float>(drawableSize.height);
                const float panelRight = (80.0f + 210.0f) * drawableSize.scale / static_cast<float>(drawableSize.width);
                const float panelBottom = (80.0f + 95.0f) * drawableSize.scale / static_cast<float>(drawableSize.height);
                ImGui::GetWindowDrawList()->AddImage((ImTextureID)targets.UiOverlay(), panelPosition,
                                                      ImVec2(panelPosition.x + panelSize.x, panelPosition.y + panelSize.y),
                                                      ImVec2(panelLeft, panelTop), ImVec2(panelRight, panelBottom));
                ImGui::Text("Metal reference path");
                ImGui::Text("Particles: %u", ParticleSaturn::Gpu::Metal::MetalParticleSystem::ParticleCount);
                ImGui::End();
                ImGui::Render();
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), (id<MTLCommandBuffer>)commands,
                                               (id<MTLRenderCommandEncoder>)encoder);
            });
        });
        ImGui_ImplMetal_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();
    }
    return 0;
}
