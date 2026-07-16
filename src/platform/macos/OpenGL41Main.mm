#import <Cocoa/Cocoa.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_osx.h"

#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/opengl41/OpenGL41Surface.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLSevenSegmentFps.h"
#include "gpu/backends/opengl41/OpenGLStarField.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"

namespace {

std::filesystem::path ShaderDirectory() {
    const char* resourcePath = [[[NSBundle mainBundle] resourcePath] UTF8String];
    return resourcePath == nullptr ? std::filesystem::path{} : std::filesystem::path{resourcePath} / "glsl410";
}

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

class WindowGlass {
public:
    WindowGlass(NSWindow* window, NSView* openGlView,
                ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface& surface)
        : window_{window}, openGlView_{openGlView}, surface_{surface} {}

    ~WindowGlass() { SetEnabled(false); }

    void SetEnabled(bool enabled) {
        if (enabled == enabled_) return;
        if (enabled) {
            visualEffect_ = [[NSVisualEffectView alloc] initWithFrame:[[window_ contentView] bounds]];
            [visualEffect_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [visualEffect_ setMaterial:NSVisualEffectMaterialHUDWindow];
            [visualEffect_ setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
            [visualEffect_ setState:NSVisualEffectStateActive];
            [window_ setOpaque:NO];
            [window_ setBackgroundColor:NSColor.clearColor];
            [window_ setContentView:visualEffect_];
            [visualEffect_ addSubview:openGlView_];
            [openGlView_ setFrame:[visualEffect_ bounds]];
            [openGlView_ setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [openGlView_ setWantsLayer:YES];
            [[openGlView_ layer] setOpaque:NO];
            surface_.SetView(openGlView_);
            surface_.SetTransparent(true);
        } else {
            if (visualEffect_ != nil) {
                [openGlView_ removeFromSuperview];
                [window_ setContentView:openGlView_];
                [visualEffect_ release];
                visualEffect_ = nil;
                surface_.SetView(openGlView_);
            }
            [window_ setOpaque:YES];
            [window_ setBackgroundColor:NSColor.blackColor];
            [openGlView_ setWantsLayer:YES];
            [[openGlView_ layer] setOpaque:YES];
            surface_.SetTransparent(false);
        }
        enabled_ = enabled;
    }

private:
    NSWindow* window_ = nil;
    NSView* openGlView_ = nil;
    ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface& surface_;
    NSVisualEffectView* visualEffect_ = nil;
    bool enabled_ = false;
};

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
        [window setReleasedWhenClosed:NO];
        auto* view = [[NSView alloc] initWithFrame:frame];
        [view setWantsLayer:YES];
        [window setContentView:view];

        auto surface = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface>(view);
        if (!surface->MakeCurrent()) return 1;
        auto glass = std::make_shared<WindowGlass>(window, view, *surface);
        const auto shaderDirectory = ShaderDirectory();
        auto particles = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem>();
        auto stars = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLStarField>();
        auto targets = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets>();
        auto bloom = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLBloom>();
        auto toneMapper = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLToneMapper>();
        auto sevenSegment = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLSevenSegmentFps>();
        if (!particles->Initialize((shaderDirectory / "ParticleSimulationTF.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.frag").c_str()) ||
            !bloom->Initialize(shaderDirectory.c_str()) || !toneMapper->Initialize(shaderDirectory.c_str()) ||
            !sevenSegment->Initialize(shaderDirectory.c_str())) return 1;
        if (!stars->Initialize((shaderDirectory / "StarRender.vert").c_str(),
                               (shaderDirectory / "StarRender.frag").c_str())) return 1;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        if (!ImGui_ImplOSX_Init(view) || !ImGui_ImplOpenGL3_Init("#version 410")) return 1;

        auto controller = std::make_shared<ParticleSaturn::App::AppController>();
        controller->MutableState().render.graphicsApi = ParticleSaturn::App::GraphicsApi::OpenGL41;
        auto coordinator = std::make_shared<ParticleSaturn::App::FrameCoordinator>();
        auto fpsMeter = std::make_shared<FpsMeter>();
        auto lastFrame = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());

        [NSTimer scheduledTimerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer*) {
            if (!surface->MakeCurrent()) return;
            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::clamp(std::chrono::duration<float>(now - *lastFrame).count(), 0.0f, 0.25f);
            *lastFrame = now;
            fpsMeter->AddSample(deltaTime);
            const auto frameSnapshot = coordinator->Advance(*controller, deltaTime);
            const auto& state = *frameSnapshot.state;
            const float backingScale = [window backingScaleFactor];
            const NSSize logicalSize = [view bounds].size;
            const auto width = static_cast<std::uint32_t>(std::max(1.0, logicalSize.width * backingScale));
            const auto height = static_cast<std::uint32_t>(std::max(1.0, logicalSize.height * backingScale));
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
            stars->Draw(static_cast<float>(state.scene.simulationTimeSeconds), width, height);
            if (!state.scene.paused) particles->Simulate(deltaTime, state.scene.zoom, false);
            particles->DrawIndirect(static_cast<float>(state.scene.simulationTimeSeconds), width, height,
                                    state.scene.zoom, state.scene.rotationX, state.scene.rotationY,
                                    state.render.pixelRatio, state.render.densityCompensation,
                                    state.render.particleCount);
            glDisable(GL_BLEND);
            const bool transparent = state.window.material == ParticleSaturn::App::WindowMaterial::SystemBlur;
            if (!bloom->Apply(*targets, state.render.bloomBlurStrength) ||
                !toneMapper->Apply(*targets, state.render.bloomEnabled ? 0.5f : 0.0f, transparent)) return;
            if (state.ui.blurEnabled && !bloom->ApplyUiBlur(*targets, state.ui.blurStrength)) return;

            glBindFramebuffer(GL_READ_FRAMEBUFFER, targets->ToneMappedFramebuffer());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (!sevenSegment->Render(0, width, height, fpsMeter->Value())) return;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplOSX_NewFrame(view);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(80.0f, 80.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(320.0f, 280.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::Begin("Particle Saturn");
            const ImVec2 panelPosition = ImGui::GetWindowPos();
            const ImVec2 panelSize = ImGui::GetWindowSize();
            if (state.ui.blurEnabled) {
                const float left = panelPosition.x / static_cast<float>(logicalSize.width);
                const float top = panelPosition.y / static_cast<float>(logicalSize.height);
                const float right = (panelPosition.x + panelSize.x) / static_cast<float>(logicalSize.width);
                const float bottom = (panelPosition.y + panelSize.y) / static_cast<float>(logicalSize.height);
                const ImTextureID texture = static_cast<ImTextureID>(targets->BloomStrongTexture());
                ImGui::GetWindowDrawList()->AddImage(texture, panelPosition,
                    ImVec2(panelPosition.x + panelSize.x, panelPosition.y + panelSize.y),
                    ImVec2(left, 1.0f - top), ImVec2(right, 1.0f - bottom));
            }
            ImGui::Text("OpenGL 4.1");
            ImGui::SameLine();
            ImGui::Text("FPS: %u", fpsMeter->Value());
            ImGui::Separator();
            ImGui::Text("Particles: %u", state.render.particleCount);
            int particleCount = static_cast<int>(state.render.particleCount);
            if (ImGui::SliderInt("Particle count", &particleCount,
                                 static_cast<int>(ParticleSaturn::App::RenderSettings::MinParticles),
                                 static_cast<int>(ParticleSaturn::App::RenderSettings::MaxParticles))) {
                controller->Dispatch(ParticleSaturn::App::SetParticleCount{static_cast<std::uint32_t>(particleCount)});
            }
            bool bloomEnabled = state.render.bloomEnabled;
            if (ImGui::Checkbox("Bloom", &bloomEnabled)) {
                controller->Dispatch(ParticleSaturn::App::SetBloomEnabled{bloomEnabled});
            }
            bool blurEnabled = state.ui.blurEnabled;
            if (ImGui::Checkbox("UI blur", &blurEnabled)) {
                controller->Dispatch(ParticleSaturn::App::SetBlurEnabled{blurEnabled});
            }
            bool glassEnabled = state.window.material == ParticleSaturn::App::WindowMaterial::SystemBlur;
            if (ImGui::Checkbox("Window glass", &glassEnabled)) {
                const auto material = glassEnabled ? ParticleSaturn::App::WindowMaterial::SystemBlur
                                                   : ParticleSaturn::App::WindowMaterial::Solid;
                controller->Dispatch(ParticleSaturn::App::SetWindowMaterial{material});
                glass->SetEnabled(glassEnabled);
            }
            float blurStrength = state.ui.blurStrength;
            if (ImGui::SliderFloat("Blur strength", &blurStrength, 0.0f, 5.0f)) {
                controller->Dispatch(ParticleSaturn::App::SetBlurStrength{blurStrength});
            }
            if (ImGui::Button(state.scene.paused ? "Resume" : "Pause")) {
                controller->Dispatch(ParticleSaturn::App::TogglePause{});
            }
            ImGui::SameLine();
            if (ImGui::Button("Fullscreen")) {
                const bool fullscreen = !state.window.fullscreen;
                controller->Dispatch(ParticleSaturn::App::SetFullscreen{fullscreen});
                [window toggleFullScreen:nil];
            }
            ImGui::End();
            ImGui::Render();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            surface->Present();
        }];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        surface->MakeCurrent();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOSX_Shutdown();
        ImGui::DestroyContext();
        glass->SetEnabled(false);
        glass.reset();
        [view release];
        [window release];
    }
    return 0;
}
