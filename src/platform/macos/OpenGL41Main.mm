#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_osx.h"

#include "MacOSApplication.h"
#include "AppShell.h"
#include "CocoaHost.h"
#include "SmokeHarness.h"
#include "MacOSMd3Panel.h"
#include "MD3.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/opengl41/OpenGL41Surface.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
#include "gpu/backends/opengl41/OpenGLFrameRenderer.h"
#include "gpu/backends/opengl41/OpenGLParticleSystem.h"
#include "gpu/backends/opengl41/OpenGLRenderTargets.h"
#include "gpu/backends/opengl41/OpenGLSevenSegmentFps.h"
#include "gpu/backends/opengl41/OpenGLStarField.h"
#include "gpu/backends/opengl41/OpenGLToneMapper.h"
#include "services/settings/macos/NSUserDefaultsStore.h"
#include "services/camera/macos/AVFoundationCamera.h"
#include "services/camera/macos/CameraSelectorWindow.h"
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"
#include "services/resources/macos/BundleResources.h"

@interface ParticleSaturnOpenGLMenuTarget : NSObject {
@public std::function<void(ParticleSaturn::Platform::MacOS::HostAction)> action;
}
- (void)quitApplication:(id)sender;
- (void)toggleDebugWindow:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)toggleBlur:(id)sender;
- (void)togglePause:(id)sender;
- (void)showCameraSelector:(id)sender;
@end

@implementation ParticleSaturnOpenGLMenuTarget
// Cmd+Q 停运行循环而非 terminate:，让 main 尾部的清理与退出码传播执行。
- (void)quitApplication:(id)sender { (void)sender; ParticleSaturn::Platform::MacOS::CocoaHost::StopRunLoop(); }
- (void)toggleDebugWindow:(id)sender { (void)sender; action(ParticleSaturn::Platform::MacOS::HostAction::ToggleDebugWindow); }
- (void)toggleFullscreen:(id)sender { (void)sender; action(ParticleSaturn::Platform::MacOS::HostAction::ToggleFullscreen); }
- (void)toggleBlur:(id)sender { (void)sender; action(ParticleSaturn::Platform::MacOS::HostAction::ToggleBlur); }
- (void)togglePause:(id)sender { (void)sender; action(ParticleSaturn::Platform::MacOS::HostAction::TogglePause); }
- (void)showCameraSelector:(id)sender { (void)sender; action(ParticleSaturn::Platform::MacOS::HostAction::ShowCameraSelector); }
@end

namespace {

void AddOpenGLMenuAction(NSMenu* menu, NSString* title, SEL selector, id target, NSString* keyEquivalent = @"") {
    auto* item = [[NSMenuItem alloc] initWithTitle:title action:selector keyEquivalent:keyEquivalent];
    [item setTarget:target];
    [menu addItem:item];
    [item release];
}

void InstallOpenGLApplicationMenu(ParticleSaturnOpenGLMenuTarget* target) {
    auto* mainMenu = [[NSMenu alloc] initWithTitle:@"Particle Saturn"];
    auto* appItem = [[NSMenuItem alloc] initWithTitle:@"Particle Saturn" action:nil keyEquivalent:@""];
    auto* appMenu = [[NSMenu alloc] initWithTitle:@"Particle Saturn"];
    AddOpenGLMenuAction(appMenu, @"Quit Particle Saturn", @selector(quitApplication:), target, @"q");
    [appItem setSubmenu:appMenu]; [appMenu release]; [mainMenu addItem:appItem]; [appItem release];
    auto* viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    auto* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    AddOpenGLMenuAction(viewMenu, @"Show or Hide Control Panel", @selector(toggleDebugWindow:), target);
    AddOpenGLMenuAction(viewMenu, @"Enter or Exit Full Screen", @selector(toggleFullscreen:), target);
    AddOpenGLMenuAction(viewMenu, @"Toggle UI Blur", @selector(toggleBlur:), target);
    [viewItem setSubmenu:viewMenu]; [viewMenu release]; [mainMenu addItem:viewItem]; [viewItem release];
    auto* controlsItem = [[NSMenuItem alloc] initWithTitle:@"Controls" action:nil keyEquivalent:@""];
    auto* controlsMenu = [[NSMenu alloc] initWithTitle:@"Controls"];
    AddOpenGLMenuAction(controlsMenu, @"Pause or Resume", @selector(togglePause:), target);
    AddOpenGLMenuAction(controlsMenu, @"Select Camera...", @selector(showCameraSelector:), target);
    [controlsItem setSubmenu:controlsMenu]; [controlsMenu release]; [mainMenu addItem:controlsItem]; [controlsItem release];
    [NSApp setMainMenu:mainMenu];
    [mainMenu release];
}

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

bool WriteBaselinePpm(const char* path, std::uint32_t framebuffer, std::uint32_t width, std::uint32_t height) {
    if (path == nullptr || path[0] == '\0' || width == 0 || height == 0) return false;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR) return false;
    std::ofstream output{path, std::ios::binary};
    if (!output) return false;
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t sourceRow = height - 1U - row;
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto* pixel = &pixels[(static_cast<std::size_t>(sourceRow) * width + column) * 4U];
            output.write(reinterpret_cast<const char*>(pixel), 3);
        }
    }
    return output.good();
}

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
            // NSOpenGLContext owns its drawable.  A layer-backed NSView forces
            // that drawable opaque on current macOS drivers, hiding the visual
            // effect view below it even when the OpenGL pixels carry alpha.
            [openGlView_ setWantsLayer:NO];
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
            [openGlView_ setWantsLayer:NO];
            surface_.SetTransparent(false);
        }
        enabled_ = enabled;
    }

    bool IsEnabled() const noexcept { return enabled_; }
    bool IsTransparent() const noexcept { return transparent_; }

    void ApplyMaterial(ParticleSaturn::App::WindowMaterial material, bool fullscreen) {
        if (material == ParticleSaturn::App::WindowMaterial::SystemBlur && !fullscreen) {
            SetEnabled(true);
            transparent_ = true;
            return;
        }
        SetEnabled(false);
        transparent_ = material == ParticleSaturn::App::WindowMaterial::Transparent && !fullscreen;
        [window_ setOpaque:!transparent_];
        [window_ setBackgroundColor:transparent_ ? NSColor.clearColor : NSColor.blackColor];
        surface_.SetTransparent(transparent_);
    }

    void PresentFullscreenBackdrop() {
        SetEnabled(false);
        // The system takes its full-screen transition snapshot before the GL
        // drawable is reliably visible.  Set the native window opaque first so
        // that snapshot cannot expose AppKit's blue backing.
        [window_ setOpaque:YES];
        [window_ setBackgroundColor:NSColor.blackColor];
        transparent_ = false;
        surface_.SetTransparent(false);
        if (!surface_.MakeCurrent()) return;
        const NSSize bounds = [openGlView_ bounds].size;
        const CGFloat scale = [window_ backingScaleFactor];
        const GLsizei width = static_cast<GLsizei>(bounds.width * scale);
        const GLsizei height = static_cast<GLsizei>(bounds.height * scale);
        if (width <= 0 || height <= 0) return;
        GLboolean scissorEnabled = GL_FALSE;
        glGetBooleanv(GL_SCISSOR_TEST, &scissorEnabled);
        if (scissorEnabled == GL_TRUE) glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (scissorEnabled == GL_TRUE) glEnable(GL_SCISSOR_TEST);
        surface_.Present();
        [window_ displayIfNeeded];
        [CATransaction flush];
    }

private:
    NSWindow* window_ = nil;
    NSView* openGlView_ = nil;
    ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface& surface_;
    NSVisualEffectView* visualEffect_ = nil;
    bool enabled_ = false;
    bool transparent_ = false;
};

// GL41 保留自建 NSOpenGL 窗口栈（材质/全屏行为不因合并改变），以 AppHost 接入外壳。
class OpenGLAppHost final : public ParticleSaturn::Platform::MacOS::AppHost {
public:
    OpenGLAppHost(NSWindow* window, NSView* view,
                  std::shared_ptr<ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface> surface,
                  std::shared_ptr<WindowGlass> glass)
        : window_{window}, view_{view}, surface_{std::move(surface)}, glass_{std::move(glass)} {}

    ParticleSaturn::Platform::MacOS::DrawableSize CurrentDrawableSize() override {
        const NSRect backingBounds = [view_ convertRectToBacking:[view_ bounds]];
        return {static_cast<std::uint32_t>(std::max(1.0, backingBounds.size.width)),
                static_cast<std::uint32_t>(std::max(1.0, backingBounds.size.height)),
                static_cast<float>([window_ backingScaleFactor])};
    }
    void WindowPosition(std::int32_t& x, std::int32_t& y) override {
        const NSPoint origin = [window_ frame].origin;
        x = static_cast<std::int32_t>(origin.x);
        y = static_cast<std::int32_t>(origin.y);
    }
    bool NativeFullscreen() override { return ([window_ styleMask] & NSWindowStyleMaskFullScreen) != 0; }
    void ToggleFullscreen() override {
        if (!NativeFullscreen()) glass_->PresentFullscreenBackdrop();
        [window_ toggleFullScreen:nil];
    }
    void SetWindowMaterial(ParticleSaturn::App::WindowMaterial material) override {
        glass_->ApplyMaterial(material, NativeFullscreen());
    }
    void SetPresentationMode(int vsyncMode) override {
        if (surface_->MakeCurrent()) surface_->SetVSyncMode(vsyncMode);
    }
    void RequestExit() override { ParticleSaturn::Platform::MacOS::CocoaHost::StopRunLoop(); }
    void Show() override {
        [window_ makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    void Run(const std::function<void()>& frameCallback) override {
        if (frameCallback) {
            auto* callback = new std::function<void()>{frameCallback};
            const NSInteger refreshRate = std::max<NSInteger>(1, [[window_ screen] maximumFramesPerSecond]);
            NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / static_cast<double>(refreshRate)
                                                     repeats:YES
                                                       block:^(NSTimer* timer) {
                if (![NSApp isRunning]) {
                    [timer invalidate];
                    delete callback;
                    return;
                }
                (*callback)();
            }];
            [timer setTolerance:0.0];
            [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSRunLoopCommonModes];
        }
        [NSApp run];
    }
    void SetActionCallback(std::function<void(ParticleSaturn::Platform::MacOS::HostAction)> callback) override {
        action_ = std::move(callback);
    }
    void InvokeAction(ParticleSaturn::Platform::MacOS::HostAction action) {
        if (action_) action_(action);
    }

private:
    NSWindow* window_ = nil;
    NSView* view_ = nil;
    std::shared_ptr<ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface> surface_;
    std::shared_ptr<WindowGlass> glass_;
    std::function<void(ParticleSaturn::Platform::MacOS::HostAction)> action_;
};

} // namespace

int ParticleSaturn::Platform::MacOS::RunOpenGL41Application() {
    @autoreleasepool {
        InstallDebugLogCapture();
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        __block ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
        const auto smoke = ParticleSaturn::Platform::MacOS::SmokeConfig::FromEnvironment();
        const bool captureBaseline = smoke.captureBaseline;
        auto initialState = smoke.Deterministic() ? ParticleSaturn::App::AppState{} : settings.Load({});
        smoke.ForceInitialState(initialState);
        const auto startup = ParticleSaturn::Platform::MacOS::ResolveStartupGeometry(initialState);
        const auto startupWidth = startup.width;
        const auto startupHeight = startup.height;
        const auto startupX = startup.x;
        const auto startupY = startup.y;
        const NSRect visibleFrame = [[NSScreen mainScreen] visibleFrame];
        const NSSize maximumContentSize = [NSWindow contentRectForFrameRect:visibleFrame
                                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable].size;
        const NSRect frame = NSMakeRect(captureBaseline ? 0 : startupX, captureBaseline ? 0 : startupY,
                                        std::min<CGFloat>(startupWidth, maximumContentSize.width),
                                        std::min<CGFloat>(startupHeight, maximumContentSize.height));
        const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                           NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        auto* window = [[NSWindow alloc] initWithContentRect:frame styleMask:style backing:NSBackingStoreBuffered defer:NO];
        [window setTitle:@"Particle Saturn - OpenGL 4.1"];
        [window setReleasedWhenClosed:NO];
        auto* view = [[NSView alloc] initWithFrame:frame];
        [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [window setContentView:view];

        auto surface = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGL41Surface>(view);
        if (!surface->MakeCurrent()) return 1;
        if (!surface->SetVSyncMode(initialState.render.vsyncMode)) return 1;
        auto glass = std::make_shared<WindowGlass>(window, view, *surface);
        glass->ApplyMaterial(initialState.window.material, false);
        const auto shaderDirectory = ShaderDirectory();
        auto particles = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem>();
        auto stars = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLStarField>();
        auto targets = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLRenderTargets>();
        auto bloom = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLBloom>();
        auto toneMapper = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLToneMapper>();
        auto sevenSegment = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLSevenSegmentFps>();
        auto frameRenderer = std::make_shared<ParticleSaturn::Gpu::OpenGL41::OpenGLFrameRenderer>();
        if (!particles->Initialize((shaderDirectory / "ParticleSimulationTF.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.vert").c_str(),
                                   (shaderDirectory / "ParticleRender.frag").c_str()) ||
            !bloom->Initialize(shaderDirectory.c_str()) || !toneMapper->Initialize(shaderDirectory.c_str()) ||
            !sevenSegment->Initialize(shaderDirectory.c_str())) return 1;
        if (!stars->Initialize((shaderDirectory / "StarRender.vert").c_str(),
                               (shaderDirectory / "StarRender.frag").c_str())) return 1;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().FontDefault = ImGui::GetIO().Fonts->AddFontFromFileTTF("/System/Library/Fonts/SFNS.ttf", 15.0f);
        ImGui::GetStyle().WindowRounding = 12.0f;
        ImGui::GetStyle().WindowBorderSize = 0.0f;
        if (!ImGui_ImplOSX_Init(view) || !ImGui_ImplOpenGL3_Init("#version 410")) return 1;

        auto controller = std::make_shared<ParticleSaturn::App::AppController>(initialState);
        MD3::Init(1.0f, true);
        MD3::SetDarkMode(controller->State().ui.darkMode);
        controller->MutableState().window.fullscreen = false;

        auto appHost = std::make_shared<OpenGLAppHost>(window, view, surface, glass);

        // 菜单/按键/窗口通知统一转发外壳动作回调（与 CocoaHost 同构）；
        // 表面/材质的宿主级处理保留在本文件。
        auto* menuTarget = [[ParticleSaturnOpenGLMenuTarget alloc] init];
        menuTarget->action = [appHost](ParticleSaturn::Platform::MacOS::HostAction action) {
            appHost->InvokeAction(action);
        };
        InstallOpenGLApplicationMenu(menuTarget);
        id closeObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification object:window queue:nil
            usingBlock:^(NSNotification*) { ParticleSaturn::Platform::MacOS::CocoaHost::StopRunLoop(); }];
        id fullscreenExitObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidExitFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) {
                surface->MakeCurrent();
                surface->UpdateDrawable();
                glass->ApplyMaterial(controller->State().window.material, false);
                appHost->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::NativeFullscreenExited);
            }];
        id fullscreenWillEnterObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillEnterFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) { glass->PresentFullscreenBackdrop(); }];
        id fullscreenEnterObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidEnterFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) {
                surface->MakeCurrent();
                surface->UpdateDrawable();
                appHost->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::NativeFullscreenEntered);
            }];
        id resizeObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidResizeNotification object:window queue:nil
            usingBlock:^(NSNotification*) { surface->MakeCurrent(); surface->UpdateDrawable(); }];
        id backingScaleObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidChangeBackingPropertiesNotification object:window queue:nil
            usingBlock:^(NSNotification*) { surface->MakeCurrent(); surface->UpdateDrawable(); }];
        id eventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent*(NSEvent* event) {
            if ([event type] == NSEventTypeKeyDown && [event isARepeat]) return nil;
            const bool pressed = [event type] == NSEventTypeKeyDown;
            using ParticleSaturn::Platform::MacOS::HostAction;
            switch ([event keyCode]) {
            case 99:
                appHost->InvokeAction(pressed ? HostAction::KeyF3Down : HostAction::KeyF3Up);
                return nil;
            case 103:
                appHost->InvokeAction(pressed ? HostAction::KeyF11Down : HostAction::KeyF11Up);
                return nil;
            case 11:
                appHost->InvokeAction(pressed ? HostAction::KeyBDown : HostAction::KeyBUp);
                return nil;
            case 53:
                appHost->InvokeAction(pressed ? HostAction::KeyEscapeDown : HostAction::KeyEscapeUp);
                return nil;
            default:
                return event;
            }
        }];

        bool baselineCaptured = false;
        std::uint32_t baselineFrameCount = 0;
        ParticleSaturn::Platform::MacOS::SmokeHarness smokeHarness{smoke, startup, "opengl41", {
            [window] { [window toggleFullScreen:nil]; },
            [] { ParticleSaturn::Platform::MacOS::CocoaHost::StopRunLoop(); }}};

        ParticleSaturn::Platform::MacOS::RunAppConfig shellConfig{
            *appHost, *controller, settings, smoke, smokeHarness, startup,
            "OpenGL 4.1", true,
            /*persistSettings=*/!smoke.Deterministic(),
            /*cameraEnabled=*/!smoke.Deterministic(),
            /*fixedDeltaTime=*/0.0f, {}, {}};
        shellConfig.renderFrame = [&](const ParticleSaturn::Platform::MacOS::FrameContext& frame) {
            if (!surface->MakeCurrent()) return false;
            const auto& state = frame.state;
            particles->SetSimulationMode(state.render.analyticParticles
                ? ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::Analytic
                : ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::TransformFeedback);
            const auto width = frame.drawableSize.width;
            const auto height = frame.drawableSize.height;
            const float backingScale = frame.drawableSize.scale;
            const float logicalWidth = std::max(1.0f, static_cast<float>(state.window.width));
            const float logicalHeight = std::max(1.0f, static_cast<float>(state.window.height));
            const bool transparent = glass->IsTransparent();
            ParticleSaturn::Gpu::OpenGL41::OpenGLFrameCallbacks callbacks;
            callbacks.capture = [&](std::uint32_t framebuffer, std::uint32_t captureWidth, std::uint32_t captureHeight) {
                if (!smoke.captureBaseline || baselineCaptured) return true;
                if (++baselineFrameCount < 3U) return false;
                if (!WriteBaselinePpm(smoke.baselinePath, framebuffer, captureWidth, captureHeight)) return false;
                baselineCaptured = true;
                ParticleSaturn::Platform::MacOS::CocoaHost::StopRunLoop();
                return false;
            };
            callbacks.renderUi = [&](std::uint32_t strongBlurTexture, std::uint32_t weakBlurTexture) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplOSX_NewFrame(view);
                ImGui::NewFrame();
                MD3::BeginFrame(frame.deltaTime);
                MD3::SetDpiScale(1.0f);
                MD3::SetScreenSize(logicalWidth, logicalHeight);
                MD3::SetBlurTexture(state.ui.blurEnabled ? strongBlurTexture : 0, state.ui.blurEnabled);
                MD3::SetBlurTexture2(state.ui.blurEnabled ? weakBlurTexture : 0);
                ParticleSaturn::Platform::MacOS::BackendPanelHooks hooks;
                hooks.drawAcrylicBackground = [&](ImDrawList* drawList, const ImVec2& position,
                                                  const ImVec2& panelSize, float rounding) {
                    const float left = position.x / logicalWidth;
                    const float top = position.y / logicalHeight;
                    const float right = (position.x + panelSize.x) / logicalWidth;
                    const float bottom = (position.y + panelSize.y) / logicalHeight;
                    MD3::AddImageRounded(drawList, reinterpret_cast<void*>(static_cast<uintptr_t>(strongBlurTexture)),
                                         position, ImVec2(position.x + panelSize.x, position.y + panelSize.y),
                                         ImVec2(left, 1.0f - top), ImVec2(right, 1.0f - bottom), IM_COL32_WHITE,
                                         rounding * backingScale);
                };
                frame.drawPanel(hooks);
                MD3::EndFrame();
                ImGui::Render();
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glViewport(0, 0, width, height);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                return glGetError() == GL_NO_ERROR;
            };
            callbacks.present = [&] { surface->Present(); };
            return frameRenderer->Render(*particles, *stars, *targets, *bloom, *toneMapper, *sevenSegment,
                                         width, height, state, frame.handTracked, frame.deltaTime,
                                         frame.framesPerSecond, transparent, callbacks);
        };
        const int exitCode = ParticleSaturn::Platform::MacOS::RunApp(shellConfig);
        surface->MakeCurrent();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOSX_Shutdown();
        MD3::Shutdown();
        ImGui::DestroyContext();
        [NSEvent removeMonitor:eventMonitor];
        [[NSNotificationCenter defaultCenter] removeObserver:fullscreenExitObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:fullscreenWillEnterObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:fullscreenEnterObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:resizeObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:backingScaleObserver];
        [[NSNotificationCenter defaultCenter] removeObserver:closeObserver];
        [menuTarget release];
        glass->SetEnabled(false);
        glass.reset();
        [view release];
        [window release];
        return exitCode;
    }
}
