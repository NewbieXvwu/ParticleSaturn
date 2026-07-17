#import <Cocoa/Cocoa.h>

#include "gpu/backends/diligent/DiligentVulkanAdapter.h"
#include "platform/macos/CocoaHost.h"

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
        const float color[] = {0.0f, 0.0f, 0.0f, 1.0f};
        assert(adapter.PresentClearFrame(color, 1));
        assert(adapter.ResizeSwapChain(drawable.width, drawable.height));
        adapter.Shutdown();
    }
    return 0;
}
