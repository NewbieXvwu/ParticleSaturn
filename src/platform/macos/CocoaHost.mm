#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <Metal/Metal.h>

#include "CocoaHost.h"

#include <algorithm>

@interface ParticleSaturnCocoaHostDelegate : NSObject<NSWindowDelegate> {
@public ParticleSaturn::Platform::MacOS::CocoaHost* owner;
}
@end

@implementation ParticleSaturnCocoaHostDelegate
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) owner->RequestExit();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) owner->SetFullscreenActive(true);
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) owner->SetFullscreenActive(false);
}
@end

namespace ParticleSaturn::Platform::MacOS {

CocoaHost::CocoaHost(std::uint32_t width, std::uint32_t height, const char* title) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    const NSRect frame = NSMakeRect(0, 0, width, height);
    const auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    auto* window = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [window setTitle:[NSString stringWithUTF8String:title]];
    [window setReleasedWhenClosed:NO];
    auto* delegate = [[ParticleSaturnCocoaHostDelegate alloc] init];
    delegate->owner = this;
    [window setDelegate:delegate];

    auto* view = [[NSView alloc] initWithFrame:frame];
    [view setWantsLayer:YES];
    auto* layer = [CAMetalLayer layer];
    [layer setContentsScale:[window backingScaleFactor]];
    [view setLayer:layer];
    [window setContentView:view];

    window_ = window;
    layer_ = layer;
    metalView_ = view;
    windowDelegate_ = delegate;
    eventMonitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        if ([event isARepeat]) return event;
        switch ([event keyCode]) {
        case 99:
            if (actionCallback_) actionCallback_(HostAction::ToggleDebugWindow);
            return nil;
        case 103:
            if (actionCallback_) actionCallback_(HostAction::ToggleFullscreen);
            return nil;
        case 11:
            if (actionCallback_) actionCallback_(HostAction::ToggleBlur);
            return nil;
        case 53:
            RequestExit();
            return nil;
        default:
            return event;
        }
    }];
}

CocoaHost::~CocoaHost() {
    if (eventMonitor_ != nullptr) [NSEvent removeMonitor:(id)eventMonitor_];
    auto* delegate = (ParticleSaturnCocoaHostDelegate*)windowDelegate_;
    if (delegate != nullptr) delegate->owner = nullptr;
    [(NSWindow*)window_ setDelegate:nil];
    [delegate release];
    [(NSVisualEffectView*)visualEffectView_ release];
    [(NSView*)metalView_ release];
    [(NSWindow*)window_ release];
}

void CocoaHost::Show() {
    auto* window = (NSWindow*)window_;
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void CocoaHost::Run(const std::function<void()>& frameCallback) {
    if (frameCallback) {
        auto* callback = new std::function<void()>{frameCallback};
        const NSInteger refreshRate = std::max<NSInteger>(1, [[(NSWindow*)window_ screen] maximumFramesPerSecond]);
        NSTimer* timer = [NSTimer timerWithTimeInterval:1.0 / static_cast<double>(refreshRate) repeats:YES block:^(NSTimer* timer) {
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

void CocoaHost::SetActionCallback(std::function<void(HostAction)> callback) {
    actionCallback_ = std::move(callback);
}

void CocoaHost::SetWindowPosition(std::int32_t x, std::int32_t y) {
    [(NSWindow*)window_ setFrameOrigin:NSMakePoint(x, y)];
}

void CocoaHost::GetWindowPosition(std::int32_t& x, std::int32_t& y) const {
    const NSPoint origin = [(NSWindow*)window_ frame].origin;
    x = static_cast<std::int32_t>(origin.x);
    y = static_cast<std::int32_t>(origin.y);
}

void CocoaHost::ToggleFullscreen() {
    if (!fullscreen_) {
        SetFullscreenActive(true);
        PresentFullscreenBackdrop();
        [(NSWindow*)window_ displayIfNeeded];
        [CATransaction flush];
    }
    [(NSWindow*)window_ toggleFullScreen:nil];
}

void CocoaHost::SetFullscreenActive(bool active) {
    fullscreen_ = active;
    SetWindowMaterial(windowMaterial_);
}

void CocoaHost::RequestExit() {
    [NSApp terminate:nil];
}

void CocoaHost::SetPresentationMode(int vsyncMode) {
    // Metal exposes display synchronization as enabled or disabled.  The
    // adaptive setting follows the synchronized path, matching FIFO fallback.
    [(CAMetalLayer*)layer_ setDisplaySyncEnabled:vsyncMode != 0];
}

void CocoaHost::SetWindowMaterial(App::WindowMaterial material) {
    windowMaterial_ = material;
    auto* window = (NSWindow*)window_;
    auto* metalView = (NSView*)metalView_;
    if (visualEffectView_ != nullptr) {
        [metalView removeFromSuperview];
        [window setContentView:metalView];
        [(NSVisualEffectView*)visualEffectView_ release];
        visualEffectView_ = nullptr;
    }
    // A behind-window visual effect has no desktop source in native fullscreen.
    // AppKit substitutes a blue full-screen backing, so retain the scene's
    // opaque black backdrop until the window returns to windowed mode.
    const bool systemBlur = material == App::WindowMaterial::SystemBlur && !fullscreen_;
    // AppKit snapshots the NSWindow while beginning the full-screen animation.
    // Every material must therefore become opaque before that snapshot, not just
    // the system-blur material.
    const bool transparent = !fullscreen_ &&
        (material == App::WindowMaterial::Transparent || systemBlur);
    [window setOpaque:!transparent && !systemBlur];
    [window setBackgroundColor:transparent || systemBlur ? NSColor.clearColor : NSColor.blackColor];
    const bool opaque = !transparent && !systemBlur;
    [metalView setWantsLayer:YES];
    auto* layer = (CAMetalLayer*)[metalView layer];
    [layer setOpaque:opaque];
    [layer setBackgroundColor:(opaque ? NSColor.blackColor : NSColor.clearColor).CGColor];
    if (!systemBlur) return;
    auto* visual = [[NSVisualEffectView alloc] initWithFrame:[[window contentView] bounds]];
    [visual setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [visual setMaterial:NSVisualEffectMaterialHUDWindow];
    [visual setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
    [visual setState:NSVisualEffectStateActive];
    [window setContentView:visual];
    [visual addSubview:metalView];
    [metalView setFrame:[visual bounds]];
    [metalView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    visualEffectView_ = visual;
}

void CocoaHost::PresentFullscreenBackdrop() {
    auto* layer = (CAMetalLayer*)layer_;
    id<MTLDevice> device = [layer device];
    if (device == nil) return;

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable == nil) return;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    auto* descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = drawable.texture;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:descriptor];
    [encoder endEncoding];
    [commands presentDrawable:drawable];
    [commands commit];
    [commands waitUntilCompleted];
    [queue release];
}

DrawableSize CocoaHost::CurrentDrawableSize() const {
    auto* window = (NSWindow*)window_;
    auto* layer = (CAMetalLayer*)layer_;
    const CGFloat scale = [window backingScaleFactor];
    const NSSize size = [[window contentView] bounds].size;
    [layer setContentsScale:scale];
    [layer setDrawableSize:CGSizeMake(size.width * scale, size.height * scale)];
    const CGSize drawableSize = [layer drawableSize];
    return {static_cast<std::uint32_t>(drawableSize.width), static_cast<std::uint32_t>(drawableSize.height), static_cast<float>(scale)};
}

void* CocoaHost::NativeMetalLayer() const noexcept {
    return layer_;
}

void* CocoaHost::NativeView() const noexcept {
    return metalView_;
}

} // namespace ParticleSaturn::Platform::MacOS
