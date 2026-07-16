#import <AVFoundation/AVFoundation.h>
#import <Cocoa/Cocoa.h>

#include "CameraSelectorWindow.h"

@interface ParticleSaturnCameraSelectorDelegate : NSObject {
@public ParticleSaturn::Services::Camera::MacOS::CameraSelectorWindow* owner;
}
- (void)startCamera:(id)sender;
- (void)refreshCameras:(id)sender;
@end

@implementation ParticleSaturnCameraSelectorDelegate
- (void)startCamera:(id)sender { (void)sender; if (owner != nullptr) owner->StartSelected(); }
- (void)refreshCameras:(id)sender { (void)sender; if (owner != nullptr) owner->Refresh(); }
@end

namespace ParticleSaturn::Services::Camera::MacOS {

namespace {

constexpr const char* SelectedCameraKey = "camera.selectedDeviceId";

void SetStatus(NSTextField* label, const std::string& text) {
    [label setStringValue:[NSString stringWithUTF8String:text.c_str()]];
}

} // namespace

CameraSelectorWindow::CameraSelectorWindow(AVFoundationCamera& camera) : camera_{camera} {
    const NSRect frame = NSMakeRect(0, 0, 460, 360);
    auto* panel = [[NSPanel alloc] initWithContentRect:frame
                                             styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
    [panel setTitle:@"Camera"];
    [panel setReleasedWhenClosed:NO];
    [panel setHidesOnDeactivate:NO];
    [panel setLevel:NSFloatingWindowLevel];
    auto* content = [panel contentView];
    auto* popup = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(20, 312, 300, 28) pullsDown:NO];
    auto* refresh = [[NSButton alloc] initWithFrame:NSMakeRect(330, 312, 110, 28)];
    [refresh setTitle:@"Refresh"];
    [refresh setBezelStyle:NSBezelStyleRounded];
    auto* start = [[NSButton alloc] initWithFrame:NSMakeRect(330, 278, 110, 28)];
    [start setTitle:@"Use camera"];
    [start setBezelStyle:NSBezelStyleRounded];
    auto* status = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 278, 300, 24)];
    [status setEditable:NO]; [status setBordered:NO]; [status setDrawsBackground:NO];
    auto* preview = [[NSView alloc] initWithFrame:NSMakeRect(20, 20, 420, 245)];
    [preview setWantsLayer:YES];
    auto* layer = [[AVCaptureVideoPreviewLayer alloc] init];
    [layer setVideoGravity:AVLayerVideoGravityResizeAspect];
    [layer setFrame:[preview bounds]];
    [preview setLayer:layer];

    auto* delegate = [[ParticleSaturnCameraSelectorDelegate alloc] init];
    delegate->owner = this;
    [refresh setTarget:delegate]; [refresh setAction:@selector(refreshCameras:)];
    [start setTarget:delegate]; [start setAction:@selector(startCamera:)];
    [content addSubview:popup]; [content addSubview:refresh]; [content addSubview:start];
    [content addSubview:status]; [content addSubview:preview];
    panel_ = panel; popup_ = popup; previewLayer_ = layer; statusLabel_ = status; delegate_ = delegate;
    Refresh();
}

CameraSelectorWindow::~CameraSelectorWindow() {
    if (delegate_ != nullptr) { ((ParticleSaturnCameraSelectorDelegate*)delegate_)->owner = nullptr; [(id)delegate_ release]; }
    [(id)previewLayer_ release];
    [(id)popup_ release]; [(id)statusLabel_ release]; [(id)panel_ release];
}

void CameraSelectorWindow::Show() {
    Refresh();
    [(NSPanel*)panel_ center];
    [(NSPanel*)panel_ makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

void CameraSelectorWindow::Refresh() {
    devices_ = camera_.Devices();
    auto* popup = (NSPopUpButton*)popup_;
    [popup removeAllItems];
    const auto preferred = [[[NSUserDefaults standardUserDefaults] stringForKey:@"camera.selectedDeviceId"] UTF8String];
    int selectedIndex = -1;
    for (std::size_t index = 0; index < devices_.size(); ++index) {
        const auto& device = devices_[index];
        [popup addItemWithTitle:[NSString stringWithUTF8String:device.name.c_str()]];
        if ((preferred != nullptr && device.id == preferred) || (!selectedDeviceId_.empty() && device.id == selectedDeviceId_)) {
            selectedIndex = static_cast<int>(index);
        }
    }
    if (selectedIndex >= 0) [popup selectItemAtIndex:selectedIndex];
    if (devices_.empty()) SetStatus((NSTextField*)statusLabel_, "No camera available");
    else SetStatus((NSTextField*)statusLabel_, "Select a camera to preview");
}

void CameraSelectorWindow::StartSelected() {
    const NSInteger index = [(NSPopUpButton*)popup_ indexOfSelectedItem];
    if (index < 0 || static_cast<std::size_t>(index) >= devices_.size()) {
        SetStatus((NSTextField*)statusLabel_, "No camera selected");
        return;
    }
    const auto& device = devices_[static_cast<std::size_t>(index)];
    if (!camera_.Start(device.id, 1280, 720)) {
        SetStatus((NSTextField*)statusLabel_, camera_.LastError());
        return;
    }
    selectedDeviceId_ = device.id;
    [[NSUserDefaults standardUserDefaults] setObject:[NSString stringWithUTF8String:device.id.c_str()]
                                              forKey:@"camera.selectedDeviceId"];
    [(AVCaptureVideoPreviewLayer*)previewLayer_ setSession:(AVCaptureSession*)camera_.NativeSession()];
    SetStatus((NSTextField*)statusLabel_, "Preview active");
}

std::string CameraSelectorWindow::SelectedDeviceId() const { return selectedDeviceId_; }

} // namespace ParticleSaturn::Services::Camera::MacOS
