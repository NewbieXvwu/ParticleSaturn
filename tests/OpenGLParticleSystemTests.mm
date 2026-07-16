#import <Cocoa/Cocoa.h>

#include "gpu/backends/opengl41/OpenGLParticleSystem.h"

#include <cassert>

int main(int argc, char* argv[]) {
    assert(argc == 2);
    @autoreleasepool {
        const NSOpenGLPixelFormatAttribute attributes[] = {
            NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion4_1Core,
            NSOpenGLPFADoubleBuffer, 0,
        };
        auto* format = [[NSOpenGLPixelFormat alloc] initWithAttributes:attributes];
        auto* context = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
        [format release];
        assert(context != nil);
        [context makeCurrentContext];
        ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem particles;
        assert(particles.Initialize(argv[1]));
        particles.Simulate(1.0f / 120.0f, 1.0f, false);
        assert(particles.RenderVertexArray() != 0);
        [context release];
    }
    return 0;
}
