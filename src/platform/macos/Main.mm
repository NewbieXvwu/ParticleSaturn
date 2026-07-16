#import <Cocoa/Cocoa.h>

#include <CoreFoundation/CoreFoundation.h>

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
        ParticleSaturn::Gpu::Metal::MetalRenderTargets targets;
        if (libraryPath == nullptr || !particles.Initialize(device, libraryPath, 0x53415455U) ||
            !targets.Create(device, size.width, size.height)) return 1;
        ParticleSaturn::Gpu::Metal::MetalFrameRenderer renderer;
        host.Show();
        host.Run([&] {
            const auto drawableSize = host.CurrentDrawableSize();
            if (drawableSize.width != size.width || drawableSize.height != size.height) {
                if (!targets.Create(device, drawableSize.width, drawableSize.height)) return;
                size = drawableSize;
            }
            renderer.Render(device, surface, particles, targets, libraryPath, drawableSize.width, drawableSize.height,
                            1.0f / 60.0f, 60);
        });
    }
    return 0;
}
