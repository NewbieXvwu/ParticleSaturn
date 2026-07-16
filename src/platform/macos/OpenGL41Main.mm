#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>

#include "gpu/backends/opengl41/OpenGL41Surface.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLStarField.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"

namespace {

std::filesystem::path ShaderDirectory() {
    const char* resourcePath = [[[NSBundle mainBundle] resourcePath] UTF8String];
    return resourcePath == nullptr ? std::filesystem::path{} : std::filesystem::path{resourcePath} / "glsl410";
}

} // namespace

int main() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        const NSRect frame = NSMakeRect(0, 0, 1280, 720);
        const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        auto* window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
        [window setTitle:@"Particle Saturn - OpenGL 4.1"];
        auto* view = [[NSView alloc] initWithFrame:frame];
        [view setWantsLayer:YES];
        [window setContentView:view];

        auto surface = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface>(view);
        if (!surface->MakeCurrent()) return 1;
        const auto shaderDirectory = ShaderDirectory();
        auto particles = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem>();
        auto stars = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLStarField>();
        auto targets = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets>();
        auto bloom = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLBloom>();
        auto toneMapper = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLToneMapper>();
        if (!particles->Initialize((shaderDirectory / "ParticleSimulationTF.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.frag").c_str()) ||
            !bloom->Initialize(shaderDirectory.c_str()) || !toneMapper->Initialize(shaderDirectory.c_str())) return 1;
        if (!stars->Initialize((shaderDirectory / "StarRender.vert").c_str(),
                               (shaderDirectory / "StarRender.frag").c_str())) return 1;

        auto lastFrame = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
        auto elapsed = std::make_shared<float>(0.0f);
        [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer*) {
            if (!surface->MakeCurrent()) return;
            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::clamp(std::chrono::duration<float>(now - *lastFrame).count(), 0.0f, 0.25f);
            *lastFrame = now;
            *elapsed += deltaTime;
            const float backingScale = [window backingScaleFactor];
            const NSSize size = [view bounds].size;
            const auto width = static_cast<std::uint32_t>(std::max(1.0, size.width * backingScale));
            const auto height = static_cast<std::uint32_t>(std::max(1.0, size.height * backingScale));
            if (targets->Width() != width || targets->Height() != height) {
                if (!targets->Create(width, height)) return;
            }

            glBindFramebuffer(GL_FRAMEBUFFER, targets->SceneFramebuffer());
            glViewport(0, 0, width, height);
            glClearColor(0.002f, 0.003f, 0.008f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_BLEND);
            glEnable(GL_PROGRAM_POINT_SIZE);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            stars->Draw(*elapsed, width, height);
            particles->Simulate(deltaTime, 1.0f, false);
            particles->DrawIndirect(*elapsed, width, height, 1.0f, 0.4f, 0.0f, 1.0f, 0.6f);
            glDisable(GL_BLEND);
            if (!bloom->Apply(*targets) || !toneMapper->Apply(*targets)) return;

            glBindFramebuffer(GL_READ_FRAMEBUFFER, targets->ToneMappedFramebuffer());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            surface->Present();
        }];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
