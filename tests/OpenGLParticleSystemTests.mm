#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"

#include <cassert>

int main(int argc, char* argv[]) {
    assert(argc == 3);
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
        ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets targets;
        assert(targets.Create(1920, 1080));
        assert(targets.SceneFramebuffer() != 0);
        assert(targets.BloomStrongFramebuffer() != 0);
        assert(targets.BloomWeakFramebuffer() != 0);
        assert(targets.SceneTexture() != 0);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glViewport(0, 0, 1920, 1080);
        particles.Simulate(1.0f / 120.0f, 1.0f, false);
        assert(glGetError() == GL_NO_ERROR);
        assert(particles.RenderVertexArray() != 0);
        assert(particles.IndirectBuffer() != 0);
        glClearColor(3.0f, 2.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        assert(glGetError() == GL_NO_ERROR);
        ParticleSaturn::Gpu::OpenGL41::OpenGLBloom bloom;
        assert(bloom.Initialize(argv[2]));
        assert(bloom.Apply(targets));
        [context release];
    }
    return 0;
}
