#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"

#include <cassert>

int main(int argc, char* argv[]) {
    assert(argc == 5);
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
        assert(particles.Initialize(argv[1], argv[2], argv[3]));
        ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets targets;
        assert(targets.Create(1920, 1080));
        assert(targets.SceneFramebuffer() != 0);
        assert(targets.BloomStrongFramebuffer() != 0);
        assert(targets.BloomWeakFramebuffer() != 0);
        assert(targets.ToneMappedFramebuffer() != 0);
        assert(targets.SceneTexture() != 0);
        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glViewport(0, 0, 1920, 1080);
        particles.Simulate(1.0f / 120.0f, 1.0f, false);
        assert(glGetError() == GL_NO_ERROR);
        assert(particles.RenderVertexArray() != 0);
        assert(particles.IndirectBuffer() != 0);
        particles.DrawIndirect();
        assert(glGetError() == GL_NO_ERROR);
        glClearColor(3.0f, 2.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        assert(glGetError() == GL_NO_ERROR);
        ParticleSaturn::Gpu::OpenGL41::OpenGLBloom bloom;
        assert(bloom.Initialize(argv[4]));
        assert(bloom.Apply(targets));
        ParticleSaturn::Gpu::OpenGL41::OpenGLToneMapper toneMapper;
        assert(toneMapper.Initialize(argv[4]));
        assert(toneMapper.Apply(targets));
        float color[4]{};
        glBindFramebuffer(GL_FRAMEBUFFER, targets.ToneMappedFramebuffer());
        glReadPixels(960, 540, 1, 1, GL_RGBA, GL_FLOAT, color);
        assert(glGetError() == GL_NO_ERROR);
        assert(color[0] > 0.5f && color[0] <= 1.0f);
        assert(color[1] > 0.5f && color[1] <= 1.0f);
        assert(color[2] > 0.5f && color[2] <= 1.0f);
        [context release];
    }
    return 0;
}
