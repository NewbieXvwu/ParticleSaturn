#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include "AVFoundationCamera.h"
#include "services/diagnostics/DiagnosticBus.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

@interface ParticleSaturnVideoDelegate : NSObject<AVCaptureVideoDataOutputSampleBufferDelegate> {
@public ParticleSaturn::Services::Camera::MacOS::AVFoundationCamera* owner;
}
@end

@implementation ParticleSaturnVideoDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    const CMTime time = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    const uint64_t timestamp = time.timescale == 0 ? 0 : static_cast<uint64_t>(time.value * 1000000000LL / time.timescale);
    if (owner != nullptr && buffer != nullptr) owner->PublishPixelBuffer(buffer, timestamp);
}
@end

namespace ParticleSaturn::Services::Camera::MacOS {

namespace Diagnostics = ParticleSaturn::Services::Diagnostics;

namespace {

Authorization ConvertAuthorization(AVAuthorizationStatus status) {
    switch (status) {
    case AVAuthorizationStatusAuthorized: return Authorization::Authorized;
    case AVAuthorizationStatusDenied: return Authorization::Denied;
    case AVAuthorizationStatusRestricted: return Authorization::Restricted;
    case AVAuthorizationStatusNotDetermined: return Authorization::NotDetermined;
    }
    return Authorization::Denied;
}

bool ConfigureDevice(AVCaptureDevice* device, std::uint32_t requestedWidth, std::uint32_t requestedHeight,
                     NSError** error) {
    AVCaptureDeviceFormat* selectedFormat = nil;
    AVFrameRateRange* selectedRange = nil;
    double selectedFrameRate = 0.0;
    double bestDistance = std::numeric_limits<double>::max();
    constexpr double requestedFrameRate = 30.0;
    for (AVCaptureDeviceFormat* format in device.formats) {
        const CMVideoDimensions dimensions = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
        for (AVFrameRateRange* range in format.videoSupportedFrameRateRanges) {
            const double negotiatedFrameRate = std::clamp(requestedFrameRate, range.minFrameRate, range.maxFrameRate);
            const auto widthDistance = static_cast<std::uint64_t>(std::abs(dimensions.width - static_cast<int>(requestedWidth)));
            const auto heightDistance = static_cast<std::uint64_t>(std::abs(dimensions.height - static_cast<int>(requestedHeight)));
            const double distance = static_cast<double>(widthDistance + heightDistance) +
                                    std::abs(negotiatedFrameRate - requestedFrameRate) * 1000.0;
            if (distance < bestDistance) {
                selectedFormat = format;
                selectedRange = range;
                selectedFrameRate = negotiatedFrameRate;
                bestDistance = distance;
            }
        }
    }
    if (selectedFormat == nil || selectedRange == nil) {
        if (error != nullptr) *error = [NSError errorWithDomain:@"ParticleSaturnCamera" code:1
            userInfo:@{NSLocalizedDescriptionKey: @"camera does not expose a usable video format"}];
        return false;
    }
    if (![device lockForConfiguration:error]) return false;
    device.activeFormat = selectedFormat;
    const CMTime duration = CMTimeMakeWithSeconds(1.0 / selectedFrameRate, 1000000000);
    device.activeVideoMinFrameDuration = duration;
    device.activeVideoMaxFrameDuration = duration;
    [device unlockForConfiguration];
    return true;
}

} // namespace

AVFoundationCamera::AVFoundationCamera() {
    disconnectObserver_ = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVCaptureDeviceWasDisconnectedNotification object:nil queue:nil
        usingBlock:^(NSNotification* notification) {
            AVCaptureDevice* device = [notification object];
            if (device != nil) HandleDeviceDisconnected([[device uniqueID] UTF8String]);
        }];
}
AVFoundationCamera::~AVFoundationCamera() {
    Stop();
    if (disconnectObserver_ != nullptr) [[NSNotificationCenter defaultCenter] removeObserver:(id)disconnectObserver_];
}

Authorization AVFoundationCamera::Permission() const { return ConvertAuthorization([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]); }
void AVFoundationCamera::RequestPermission() { [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {}]; }

std::vector<Device> AVFoundationCamera::Devices() const {
    std::vector<Device> devices;
    auto* discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal]
        mediaType:AVMediaTypeVideo
        position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* device in [discovery devices]) {
        devices.push_back({[[device uniqueID] UTF8String], [[device localizedName] UTF8String], [device isConnected]});
    }
    return devices;
}

bool AVFoundationCamera::Start(const std::string& deviceId, std::uint32_t width, std::uint32_t height) {
    Stop();
    if (Permission() != Authorization::Authorized) {
        std::lock_guard lock{mutex_};
        error_ = "camera permission has not been granted";
        Diagnostics::DiagnosticBus::Instance().Publish("camera", "permission", error_, Diagnostics::Severity::Error);
        return false;
    }
    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:deviceId.c_str()]];
    if (device == nil || ![device isConnected]) {
        std::lock_guard lock{mutex_};
        error_ = "selected camera is unavailable";
        Diagnostics::DiagnosticBus::Instance().Publish("camera", "device-unavailable", error_, Diagnostics::Severity::Error);
        return false;
    }
    NSError* error = nil;
    if (!ConfigureDevice(device, width, height, &error)) {
        std::lock_guard lock{mutex_};
        error_ = [[error localizedDescription] UTF8String];
        Diagnostics::DiagnosticBus::Instance().Publish("camera", "format", error_, Diagnostics::Severity::Error);
        return false;
    }
    auto* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (input == nil) {
        std::lock_guard lock{mutex_};
        error_ = [[error localizedDescription] UTF8String];
        Diagnostics::DiagnosticBus::Instance().Publish("camera", "input", error_, Diagnostics::Severity::Error);
        return false;
    }
    auto* session = [[AVCaptureSession alloc] init];
    auto* output = [[AVCaptureVideoDataOutput alloc] init];
    [output setVideoSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    [output setAlwaysDiscardsLateVideoFrames:YES];
    auto* delegate = [[ParticleSaturnVideoDelegate alloc] init];
    delegate->owner = this;
    dispatch_queue_t queue = dispatch_queue_create("com.particlesaturn.camera", DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:delegate queue:queue];
    if (![session canAddInput:input] || ![session canAddOutput:output]) {
        [delegate release]; [output release]; [session release];
        std::lock_guard lock{mutex_}; error_ = "camera session cannot accept input or output";
        Diagnostics::DiagnosticBus::Instance().Publish("camera", "session", error_, Diagnostics::Severity::Error); return false;
    }
    [session addInput:input]; [session addOutput:output];
    AVCaptureConnection* connection = [output connectionWithMediaType:AVMediaTypeVideo];
    if (connection != nil && [connection isVideoRotationAngleSupported:0.0]) {
        [connection setVideoRotationAngle:0.0];
    }
    if (connection != nil && [connection isVideoMirroringSupported]) {
        [connection setVideoMirrored:device.position == AVCaptureDevicePositionFront];
    }
    [session startRunning];
    {
        std::lock_guard lock{mutex_};
        session_ = session; output_ = output; delegate_ = delegate; activeDeviceId_ = deviceId; hasFrame_ = false; error_.clear();
    }
    return true;
}

void AVFoundationCamera::Stop() {
    std::lock_guard lock{mutex_};
    if (session_ != nullptr) [(AVCaptureSession*)session_ stopRunning];
    if (delegate_ != nullptr) { ((ParticleSaturnVideoDelegate*)delegate_)->owner = nullptr; [(id)delegate_ release]; }
    if (output_ != nullptr) [(id)output_ release];
    if (session_ != nullptr) [(id)session_ release];
    session_ = output_ = delegate_ = nullptr; activeDeviceId_.clear(); hasFrame_ = false;
}

bool AVFoundationCamera::IsRunning() const { std::lock_guard lock{mutex_}; return session_ != nullptr && [(AVCaptureSession*)session_ isRunning]; }
// 消费语义读取：hasFrame_ 置假后 latestFrame_ 不会再被读，移动而非在锁内
// 深拷贝 ~1MB 像素（AUDIT P2-8）。
bool AVFoundationCamera::LatestFrame(Frame& frame) { std::lock_guard lock{mutex_}; if (!hasFrame_) return false; frame = std::move(latestFrame_); hasFrame_ = false; return true; }
std::string AVFoundationCamera::LastError() const { std::lock_guard lock{mutex_}; return error_; }
void* AVFoundationCamera::NativeSession() const { std::lock_guard lock{mutex_}; return session_; }

void AVFoundationCamera::HandleDeviceDisconnected(const char* deviceId) {
    std::lock_guard lock{mutex_};
    if (session_ == nullptr || deviceId == nullptr || activeDeviceId_ != deviceId) return;
    [(AVCaptureSession*)session_ stopRunning];
    error_ = "active camera was disconnected";
    Diagnostics::DiagnosticBus::Instance().Publish("camera", "disconnected", error_, Diagnostics::Severity::Warning);
    hasFrame_ = false;
}

void AVFoundationCamera::PublishPixelBuffer(void* pixelBuffer, std::uint64_t timestampNanoseconds) {
    auto buffer = (CVPixelBufferRef)pixelBuffer;
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const auto width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer));
    const auto height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer));
    const auto stride = CVPixelBufferGetBytesPerRow(buffer);
    const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    Frame frame{width, height, timestampNanoseconds, width * 4U, PixelFormat::BGRA32, FrameOrientation::Up,
                false, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4U)};
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame.pixels.data() + static_cast<std::size_t>(y) * frame.bytesPerRow,
                    source + static_cast<std::size_t>(y) * stride, frame.bytesPerRow);
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    std::lock_guard lock{mutex_}; latestFrame_ = std::move(frame); hasFrame_ = true;
}

} // namespace ParticleSaturn::Services::Camera::MacOS
