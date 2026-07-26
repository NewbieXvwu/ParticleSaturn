#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#import <Metal/Metal.h>

#include "CocoaHost.h"

#include <algorithm>

@interface ParticleSaturnCocoaHostDelegate : NSObject<NSWindowDelegate> {
@public ParticleSaturn::Platform::MacOS::CocoaHost* owner;
}
- (void)quitApplication:(id)sender;
- (void)toggleDebugWindow:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)toggleBlur:(id)sender;
- (void)togglePause:(id)sender;
- (void)showCameraSelector:(id)sender;
@end

@implementation ParticleSaturnCocoaHostDelegate
// Cmd+Q 走 RequestExit：运行循环正常结束，main 尾部的设置保存与清理得以执行
// （terminate: 以退出码 0 直接杀进程，会丢失面板改动）。
- (void)quitApplication:(id)sender { (void)sender; if (owner != nullptr) owner->RequestExit(); }
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) owner->RequestExit();
}
- (void)windowDidEnterFullScreen:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetFullscreenActive(true);
        owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::NativeFullscreenEntered);
    }
}
- (void)windowDidExitFullScreen:(NSNotification*)notification {
    (void)notification;
    if (owner != nullptr) {
        owner->SetFullscreenActive(false);
        owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::NativeFullscreenExited);
    }
}
- (void)toggleDebugWindow:(id)sender { (void)sender; if (owner != nullptr) owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::ToggleDebugWindow); }
- (void)toggleFullscreen:(id)sender { (void)sender; if (owner != nullptr) owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::ToggleFullscreen); }
- (void)toggleBlur:(id)sender { (void)sender; if (owner != nullptr) owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::ToggleBlur); }
- (void)togglePause:(id)sender { (void)sender; if (owner != nullptr) owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::TogglePause); }
- (void)showCameraSelector:(id)sender { (void)sender; if (owner != nullptr) owner->InvokeAction(ParticleSaturn::Platform::MacOS::HostAction::ShowCameraSelector); }
@end

namespace ParticleSaturn::Platform::MacOS {

static void AddMenuAction(NSMenu* menu, NSString* title, SEL action, id target, NSString* keyEquivalent = @"") {
    auto* item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:keyEquivalent];
    [item setTarget:target];
    [menu addItem:item];
    [item release];
}

static void InstallApplicationMenu(ParticleSaturnCocoaHostDelegate* target) {
    auto* mainMenu = [[NSMenu alloc] initWithTitle:@"Particle Saturn"];
    auto* appItem = [[NSMenuItem alloc] initWithTitle:@"Particle Saturn" action:nil keyEquivalent:@""];
    auto* appMenu = [[NSMenu alloc] initWithTitle:@"Particle Saturn"];
    AddMenuAction(appMenu, @"Quit Particle Saturn", @selector(quitApplication:), target, @"q");
    [appItem setSubmenu:appMenu]; [appMenu release]; [mainMenu addItem:appItem]; [appItem release];
    auto* viewItem = [[NSMenuItem alloc] initWithTitle:@"View" action:nil keyEquivalent:@""];
    auto* viewMenu = [[NSMenu alloc] initWithTitle:@"View"];
    AddMenuAction(viewMenu, @"Show or Hide Control Panel", @selector(toggleDebugWindow:), target);
    AddMenuAction(viewMenu, @"Enter or Exit Full Screen", @selector(toggleFullscreen:), target);
    AddMenuAction(viewMenu, @"Toggle UI Blur", @selector(toggleBlur:), target);
    [viewItem setSubmenu:viewMenu]; [viewMenu release]; [mainMenu addItem:viewItem]; [viewItem release];
    auto* controlsItem = [[NSMenuItem alloc] initWithTitle:@"Controls" action:nil keyEquivalent:@""];
    auto* controlsMenu = [[NSMenu alloc] initWithTitle:@"Controls"];
    AddMenuAction(controlsMenu, @"Pause or Resume", @selector(togglePause:), target);
    AddMenuAction(controlsMenu, @"Select Camera...", @selector(showCameraSelector:), target);
    [controlsItem setSubmenu:controlsMenu]; [controlsMenu release]; [mainMenu addItem:controlsItem]; [controlsItem release];
    [NSApp setMainMenu:mainMenu];
    [mainMenu release];
}

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
    InstallApplicationMenu(delegate);

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
    eventMonitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent*(NSEvent* event) {
        if ([event type] == NSEventTypeKeyDown && [event isARepeat]) return nil;
        const bool pressed = [event type] == NSEventTypeKeyDown;
        switch ([event keyCode]) {
        case 99:
            if (actionCallback_) actionCallback_(pressed ? HostAction::KeyF3Down : HostAction::KeyF3Up);
            return nil;
        case 103:
            if (actionCallback_) actionCallback_(pressed ? HostAction::KeyF11Down : HostAction::KeyF11Up);
            return nil;
        case 11:
            if (actionCallback_) actionCallback_(pressed ? HostAction::KeyBDown : HostAction::KeyBUp);
            return nil;
        case 53:
            if (actionCallback_) actionCallback_(pressed ? HostAction::KeyEscapeDown : HostAction::KeyEscapeUp);
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

void CocoaHost::InvokeAction(HostAction action) {
    if (actionCallback_) actionCallback_(action);
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
    StopRunLoop();
}

void CocoaHost::StopRunLoop() {
    [NSApp stop:nil];
    // stop: 要等处理完一个真实事件才生效，而帧定时器回调不产生事件；
    // 补发一个空事件让运行循环立即醒来退出。
    @autoreleasepool {
        NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSMakePoint(0, 0)
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:wake atStart:YES];
    }
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
