#import <Cocoa/Cocoa.h>

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
        host.CurrentDrawableSize();
        host.Show();
        host.Run();
    }
    return 0;
}
