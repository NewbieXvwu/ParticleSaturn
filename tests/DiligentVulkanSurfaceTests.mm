#import <Cocoa/Cocoa.h>

#include "gpu/backends/diligent/DiligentVulkanAdapter.h"
#include "platform/macos/CocoaHost.h"
#include "imgui.h"

#include <cassert>
#include <cstring>
#include <string>

int main(int argc, char* argv[]) {
    assert(argc == 3);
    @autoreleasepool {
        ParticleSaturn::Platform::MacOS::CocoaHost host{160, 90, "Particle Saturn Vulkan Test"};
        host.Show();
        const auto drawable = host.CurrentDrawableSize();
        assert(drawable.width > 0 && drawable.height > 0);

        ParticleSaturn::Gpu::Diligent::DiligentVulkanAdapter adapter;
        std::string error;
        const auto driver = std::strcmp(argv[2], "kosmic") == 0
            ? ParticleSaturn::App::VulkanDriver::KosmicKrisp
            : ParticleSaturn::App::VulkanDriver::MoltenVK;
        assert(adapter.Initialize(driver, argv[1], error));
        assert(adapter.CreateSwapChain(host.NativeView(), drawable.width, drawable.height, error));
        assert(adapter.InitializeImGui(host.NativeView(), error));
        const float color[] = {0.0f, 0.0f, 0.0f, 1.0f};
        assert(adapter.PresentClearFrame(color, 1));
        adapter.SetParticleSettings(512, false);
        const auto presentUiFrame = [&](const char* label) {
            adapter.BeginImGuiFrame();
            ImGui::Begin("Vulkan surface test");
            ImGui::TextUnformatted(label);
            ImGui::End();
            adapter.SetAcrylicPanelRect(4.0f, 4.0f, 100.0f, 60.0f, 1.0f);
            return adapter.PresentSceneFrame(1);
        };
        ParticleSaturn::App::AppState frameState;
        frameState.scene.rotationX = 0.25f;
        frameState.scene.rotationY = -0.35f;
        frameState.scene.zoom = 1.2f;
        frameState.render.bloomBlurStrength = 2.0f;
        adapter.SetSceneSettings(frameState.scene, frameState.render);
        adapter.SetGestureState(true, 1.4f);
        adapter.SetFramesPerSecond(24);
        adapter.SetAcrylicSettings(true, 2.0f, true);
        assert(presentUiFrame(argv[2]));
        const auto resizedWidth = drawable.width > 2 ? drawable.width - 1 : drawable.width + 1;
        const auto resizedHeight = drawable.height > 2 ? drawable.height - 1 : drawable.height + 1;
        assert(adapter.ResizeSwapChain(resizedWidth, resizedHeight));
        assert(adapter.PresentClearFrame(color, 1));
        frameState.render.bloomEnabled = false;
        frameState.render.bloomBlurStrength = 0.0f;
        adapter.SetSceneSettings(frameState.scene, frameState.render);
        adapter.SetAcrylicSettings(false, 0.0f, false);
        assert(presentUiFrame("acrylic-disabled"));
        adapter.SetParticleSettings(512, true);
        adapter.SetGestureState(false, 1.0f);
        adapter.SetFramesPerSecond(120);
        frameState.render.bloomEnabled = true;
        frameState.render.bloomBlurStrength = 5.0f;
        adapter.SetSceneSettings(frameState.scene, frameState.render);
        adapter.SetAcrylicSettings(true, 4.0f, false);
        assert(presentUiFrame("acrylic-enabled"));
        adapter.Shutdown();
    }
    return 0;
}
