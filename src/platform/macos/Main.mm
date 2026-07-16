#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>

#include <CoreFoundation/CoreFoundation.h>

#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_impl_osx.h"

#include "CocoaHost.h"
#include "gpu/backends/metal/MetalBackend.h"

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
        if (!particleRenderer.Initialize(device, libraryPath)) return 1;
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplOSX_Init((NSView*)host.NativeView()) || !ImGui_ImplMetal_Init((id<MTLDevice>)device.NativeDevice())) return 1;
        host.Show();
        host.Run([&] {
            const auto drawableSize = host.CurrentDrawableSize();
            if (drawableSize.width != size.width || drawableSize.height != size.height) {
                if (!targets.Create(device, drawableSize.width, drawableSize.height)) return;
                size = drawableSize;
            }
            renderer.Render(device, surface, particles, stars, particleRenderer, targets, libraryPath, drawableSize.width, drawableSize.height,
                            drawableSize.scale, 1.0f / 60.0f, 60, [&](void* commands, void* encoder, void* pass) {
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
