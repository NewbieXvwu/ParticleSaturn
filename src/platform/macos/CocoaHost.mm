#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "CocoaHost.h"

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

    auto* view = [[NSView alloc] initWithFrame:frame];
    [view setWantsLayer:YES];
    auto* layer = [CAMetalLayer layer];
    [layer setContentsScale:[window backingScaleFactor]];
    [view setLayer:layer];
    [window setContentView:view];

    window_ = window;
    layer_ = layer;
    metalView_ = view;
}

CocoaHost::~CocoaHost() {
    [(NSVisualEffectView*)visualEffectView_ release];
    [(NSView*)metalView_ release];
    [(NSWindow*)window_ release];
}

void CocoaHost::Show() {
    auto* window = (NSWindow*)window_;
    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void CocoaHost::Run() {
    [NSApp run];
}

void CocoaHost::ToggleFullscreen() {
    [(NSWindow*)window_ toggleFullScreen:nil];
}

void CocoaHost::SetWindowMaterial(App::WindowMaterial material) {
    auto* window = (NSWindow*)window_;
    auto* metalView = (NSView*)metalView_;
    if (visualEffectView_ != nullptr) {
        [metalView removeFromSuperview];
        [window setContentView:metalView];
        [(NSVisualEffectView*)visualEffectView_ release];
        visualEffectView_ = nullptr;
    }
    const bool transparent = material == App::WindowMaterial::Transparent || material == App::WindowMaterial::AppAcrylic;
    [window setOpaque:!transparent && material != App::WindowMaterial::SystemBlur];
    [window setBackgroundColor:transparent || material == App::WindowMaterial::SystemBlur ? NSColor.clearColor : NSColor.blackColor];
    [metalView setWantsLayer:YES];
    [[metalView layer] setOpaque:!transparent];
    if (material != App::WindowMaterial::SystemBlur) return;
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

} // namespace ParticleSaturn::Platform::MacOS
