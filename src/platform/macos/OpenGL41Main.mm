#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <OpenGL/gl3.h>

#include <algorithm>
#include <array>
#include <chrono>
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
#include "CocoaHost.h"
#include "MacOSMd3Panel.h"
#include "MD3.h"
#include "app/AppController.h"
#include "app/FrameCoordinator.h"
#include "gpu/backends/opengl41/OpenGL41Surface.h"
#include "gpu/backends/opengl41/OpenGLBloom.h"
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
- (void)toggleDebugWindow:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)toggleBlur:(id)sender;
- (void)togglePause:(id)sender;
- (void)showCameraSelector:(id)sender;
@end

@implementation ParticleSaturnOpenGLMenuTarget
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
    AddOpenGLMenuAction(appMenu, @"Quit Particle Saturn", @selector(terminate:), NSApp, @"q");
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

} // namespace

int ParticleSaturn::Platform::MacOS::RunOpenGL41Application() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        __block ParticleSaturn::Services::Settings::MacOS::NSUserDefaultsStore settings;
        const char* baselinePath = std::getenv("PARTICLESATURN_CAPTURE_BASELINE");
        const bool captureBaseline = baselinePath != nullptr && baselinePath[0] != '\0';
        auto initialState = captureBaseline ? ParticleSaturn::App::AppState{} : settings.Load({});
        if (captureBaseline) {
            initialState.window.width = 1512;
            initialState.window.height = 827;
            initialState.scene.paused = true;
            initialState.lod.locked = true;
        }
        const NSRect visibleFrame = [[NSScreen mainScreen] visibleFrame];
        const NSSize maximumContentSize = [NSWindow contentRectForFrameRect:visibleFrame
                                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable].size;
        const NSRect frame = NSMakeRect(captureBaseline ? 0 : initialState.window.x, captureBaseline ? 0 : initialState.window.y,
                                        std::min<CGFloat>(initialState.window.width, maximumContentSize.width),
                                        std::min<CGFloat>(initialState.window.height, maximumContentSize.height));
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
        auto* settingsPtr = &settings;
        controller->MutableState().window.fullscreen =
            ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
        auto toggleFullscreen = [&] {
            const bool nativeFullscreen = ([window styleMask] & NSWindowStyleMaskFullScreen) != 0;
            controller->MutableState().window.fullscreen = nativeFullscreen;
            const auto effect = controller->Dispatch(ParticleSaturn::App::SetFullscreen{
                !nativeFullscreen});
            if (!effect.windowChanged) return;
            if (!nativeFullscreen) glass->PresentFullscreenBackdrop();
            [window toggleFullScreen:nil];
            settingsPtr->Save(controller->State());
        };
        id closeObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillCloseNotification object:window queue:nil
            usingBlock:^(NSNotification*) { [NSApp terminate:nil]; }];
        id fullscreenExitObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidExitFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) {
                controller->MutableState().window.fullscreen = false;
                surface->MakeCurrent();
                surface->UpdateDrawable();
                glass->ApplyMaterial(controller->State().window.material, false);
                settingsPtr->Save(controller->State());
            }];
        id fullscreenWillEnterObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowWillEnterFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) {
                controller->MutableState().window.fullscreen = true;
                glass->PresentFullscreenBackdrop();
            }];
        id fullscreenEnterObserver = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWindowDidEnterFullScreenNotification object:window queue:nil
            usingBlock:^(NSNotification*) {
                controller->MutableState().window.fullscreen = true;
                surface->MakeCurrent();
                surface->UpdateDrawable();
                settingsPtr->Save(controller->State());
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
            const auto dispatchKey = [&](ParticleSaturn::App::InputKey key) {
                const auto effect = controller->Dispatch(ParticleSaturn::App::SetInputKeyPressed{key, pressed});
                if (effect.windowChanged) toggleFullscreen();
                if (effect.exitRequested) [NSApp terminate:nil];
                if (pressed && !captureBaseline) settingsPtr->Save(controller->State());
            };
            switch ([event keyCode]) {
            case 99:
                dispatchKey(ParticleSaturn::App::InputKey::F3);
                return nil;
            case 103:
                dispatchKey(ParticleSaturn::App::InputKey::F11);
                return nil;
            case 11:
                dispatchKey(ParticleSaturn::App::InputKey::B);
                return nil;
            case 53:
                dispatchKey(ParticleSaturn::App::InputKey::Escape);
                return nil;
            default:
                return event;
            }
        }];
        __block bool baselineCaptured = false;
        __block std::uint32_t baselineFrameCount = 0;
        auto coordinator = std::make_shared<ParticleSaturn::App::FrameCoordinator>();
        auto fpsMeter = std::make_shared<FpsMeter>();
        auto lastFrame = std::make_shared<std::chrono::steady_clock::time_point>(std::chrono::steady_clock::now());
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
        auto camera = std::make_shared<ParticleSaturn::Services::Camera::MacOS::AVFoundationCamera>();
        auto cameraSelector = std::make_shared<ParticleSaturn::Services::Camera::MacOS::CameraSelectorWindow>(*camera);
        if (!captureBaseline) cameraSelector->StartSaved();
        auto handRuntime = std::make_shared<ParticleSaturn::Services::HandTracking::MacOS::XnnpackHandTrackingRuntime>();
        std::shared_ptr<ParticleSaturn::Services::HandTracking::MacOS::HandTrackingWorker> handTracking;
        std::string handTrackingError;
        if (handRuntime->Load(ParticleSaturn::Services::Resources::MacOS::LocateModel("palm_detection_full.tflite"),
                              ParticleSaturn::Services::Resources::MacOS::LocateModel("hand_landmark_full.tflite"), handTrackingError)) {
            handTracking = std::make_shared<ParticleSaturn::Services::HandTracking::MacOS::HandTrackingWorker>(
                [handRuntime](const ParticleSaturn::Services::Camera::Frame& input,
                              ParticleSaturn::Services::HandTracking::MacOS::HandPose& pose, std::string& error) {
                    return handRuntime->Invoke(input, error) && handRuntime->DecodeLandmarks(pose);
                });
            handTracking->Start();
        }
#endif
        auto* menuTarget = [[ParticleSaturnOpenGLMenuTarget alloc] init];
        menuTarget->action = [&, settingsPtr] (ParticleSaturn::Platform::MacOS::HostAction action) {
            switch (action) {
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleDebugWindow:
                controller->Dispatch(ParticleSaturn::App::ToggleDebugWindow{});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleFullscreen:
                toggleFullscreen();
                return;
            case ParticleSaturn::Platform::MacOS::HostAction::ToggleBlur:
                controller->Dispatch(ParticleSaturn::App::SetBlurEnabled{!controller->State().ui.blurEnabled});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::TogglePause:
                controller->Dispatch(ParticleSaturn::App::TogglePause{});
                break;
            case ParticleSaturn::Platform::MacOS::HostAction::ShowCameraSelector:
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
                if (camera->Permission() == ParticleSaturn::Services::Camera::Authorization::NotDetermined) camera->RequestPermission();
                cameraSelector->Show();
#endif
                break;
            default:
                break;
            }
            if (!captureBaseline) settingsPtr->Save(controller->State());
        };
        InstallOpenGLApplicationMenu(menuTarget);
        auto appliedVsyncMode = std::make_shared<int>(initialState.render.vsyncMode);

        const NSInteger refreshRate = std::max<NSInteger>(1, [[window screen] maximumFramesPerSecond]);
        NSTimer* frameTimer = [NSTimer timerWithTimeInterval:1.0 / static_cast<double>(refreshRate) repeats:YES block:^(NSTimer* timer) {
            if (![NSApp isRunning]) {
                [timer invalidate];
                return;
            }
            if (!surface->MakeCurrent()) return;
            const auto now = std::chrono::steady_clock::now();
            const float deltaTime = std::clamp(std::chrono::duration<float>(now - *lastFrame).count(), 0.0f, 0.25f);
            *lastFrame = now;
            fpsMeter->AddSample(deltaTime);
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
            ParticleSaturn::App::GestureInput gesture;
            ParticleSaturn::Services::Camera::Frame cameraFrame;
            if (handTracking && camera->LatestFrame(cameraFrame)) handTracking->Submit(std::move(cameraFrame), controller->State().gesture.handLostDelay);
            if (handTracking) gesture = handTracking->LatestGesture();
            const auto frameSnapshot = coordinator->Advance(*controller, deltaTime, gesture);
            const bool handTracked = gesture.tracked;
#else
            const auto frameSnapshot = coordinator->Advance(*controller, deltaTime);
            const bool handTracked = false;
#endif
            const auto& state = *frameSnapshot.state;
            particles->SetSimulationMode(state.render.analyticParticles
                ? ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::Analytic
                : ParticleSaturn::Gpu::OpenGL41::OpenGLParticleSystem::SimulationMode::TransformFeedback);
            if (*appliedVsyncMode != state.render.vsyncMode) {
                if (!surface->SetVSyncMode(state.render.vsyncMode)) return;
                *appliedVsyncMode = state.render.vsyncMode;
            }
            const NSSize logicalSize = [view bounds].size;
            const NSRect backingBounds = [view convertRectToBacking:[view bounds]];
            const float backingScale = [window backingScaleFactor];
            controller->MutableState().window.width = static_cast<std::uint32_t>(logicalSize.width);
            controller->MutableState().window.height = static_cast<std::uint32_t>(logicalSize.height);
            controller->MutableState().window.dpiScale = backingScale;
            if (!controller->State().window.fullscreen) {
                const NSPoint origin = [window frame].origin;
                auto& windowState = controller->MutableState().window;
                windowState.x = static_cast<std::int32_t>(origin.x);
                windowState.y = static_cast<std::int32_t>(origin.y);
                windowState.windowedX = windowState.x;
                windowState.windowedY = windowState.y;
                windowState.windowedWidth = windowState.width;
                windowState.windowedHeight = windowState.height;
            }
            const auto width = static_cast<std::uint32_t>(std::max(1.0, backingBounds.size.width));
            const auto height = static_cast<std::uint32_t>(std::max(1.0, backingBounds.size.height));
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
            if (!state.scene.paused) particles->Simulate(deltaTime, state.scene.zoom, handTracked);
            particles->DrawIndirect(static_cast<float>(state.scene.simulationTimeSeconds), width, height,
                                    state.scene.zoom, state.scene.rotationX, state.scene.rotationY,
                                    state.render.pixelRatio, state.render.densityCompensation,
                                    state.render.particleCount);
            glDisable(GL_BLEND);
            const bool transparent = glass->IsTransparent();
            if (!bloom->Apply(*targets, state.render.bloomBlurStrength) ||
                !toneMapper->Apply(*targets, state.render.bloomEnabled ? 0.5f : 0.0f, transparent)) return;
            if (captureBaseline && !baselineCaptured) {
                if (++baselineFrameCount < 3U) return;
                if (!WriteBaselinePpm(baselinePath, targets->ToneMappedFramebuffer(), width, height)) return;
                baselineCaptured = true;
                [NSApp terminate:nil];
                return;
            }
            if (state.ui.blurEnabled && !bloom->ApplyUiBlur(*targets, state.ui.blurStrength)) return;

            if (!toneMapper->Present(*targets, transparent)) return;
            if (!sevenSegment->Render(0, width, height, fpsMeter->Value())) return;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplOSX_NewFrame(view);
            ImGui::NewFrame();
            MD3::BeginFrame(deltaTime);
            MD3::SetDpiScale(1.0f);
            MD3::SetScreenSize(static_cast<float>(logicalSize.width), static_cast<float>(logicalSize.height));
            MD3::SetBlurTexture(state.ui.blurEnabled ? targets->BloomStrongTexture() : 0, state.ui.blurEnabled);
            MD3::SetBlurTexture2(state.ui.blurEnabled ? targets->BloomWeakTexture() : 0);
            ParticleSaturn::Platform::MacOS::RenderMd3Panel(*controller, "OpenGL 4.1", fpsMeter->Value(), true, {
                [&] { if (!captureBaseline) settingsPtr->Save(controller->State()); },
                [&] { toggleFullscreen(); },
                [&] {
#if defined(PARTICLESATURN_HAS_XNNPACK_RUNTIME)
                    if (camera->Permission() == ParticleSaturn::Services::Camera::Authorization::NotDetermined) camera->RequestPermission();
                    cameraSelector->Show();
#endif
                },
                [&] { if (ParticleSaturn::Platform::MacOS::RestartApplication()) [NSApp terminate:nil]; },
                [&](ParticleSaturn::App::WindowMaterial material) { glass->ApplyMaterial(material, controller->State().window.fullscreen); },
                [&](ImDrawList* drawList, const ImVec2& position, const ImVec2& panelSize) {
                    const float left = position.x / static_cast<float>(logicalSize.width);
                    const float top = position.y / static_cast<float>(logicalSize.height);
                    const float right = (position.x + panelSize.x) / static_cast<float>(logicalSize.width);
                    const float bottom = (position.y + panelSize.y) / static_cast<float>(logicalSize.height);
                    MD3::AddImageRounded(drawList, reinterpret_cast<void*>(static_cast<uintptr_t>(targets->BloomStrongTexture())), position,
                                         ImVec2(position.x + panelSize.x, position.y + panelSize.y),
                                         ImVec2(left, 1.0f - top), ImVec2(right, 1.0f - bottom), IM_COL32_WHITE,
                                         12.0f * backingScale);
                }});
            MD3::EndFrame();
            ImGui::Render();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width, height);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            surface->Present();
        }];
        [frameTimer setTolerance:0.0];
        [[NSRunLoop mainRunLoop] addTimer:frameTimer forMode:NSRunLoopCommonModes];
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
        surface->MakeCurrent();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOSX_Shutdown();
        MD3::Shutdown();
        ImGui::DestroyContext();
        if (!captureBaseline) settings.Save(controller->State());
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
    }
    return 0;
}
