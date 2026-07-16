#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>

#include "AVFoundationCamera.h"

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

} // namespace

AVFoundationCamera::AVFoundationCamera() = default;
AVFoundationCamera::~AVFoundationCamera() { Stop(); }

Authorization AVFoundationCamera::Permission() const { return ConvertAuthorization([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]); }
void AVFoundationCamera::RequestPermission() { [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {}]; }

std::vector<Device> AVFoundationCamera::Devices() const {
    std::vector<Device> devices;
    auto* discovery = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternalUnknown]
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
        return false;
    }
    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:[NSString stringWithUTF8String:deviceId.c_str()]];
    if (device == nil || ![device isConnected]) {
        std::lock_guard lock{mutex_};
        error_ = "selected camera is unavailable";
        return false;
    }
    NSError* error = nil;
    auto* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (input == nil) {
        std::lock_guard lock{mutex_};
        error_ = [[error localizedDescription] UTF8String];
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
        std::lock_guard lock{mutex_}; error_ = "camera session cannot accept input or output"; return false;
    }
    [session addInput:input]; [session addOutput:output];
    [session startRunning];
    {
        std::lock_guard lock{mutex_};
        session_ = session; output_ = output; delegate_ = delegate; hasFrame_ = false; error_.clear();
    }
    (void)width; (void)height;
    return true;
}

void AVFoundationCamera::Stop() {
    std::lock_guard lock{mutex_};
    if (session_ != nullptr) [(AVCaptureSession*)session_ stopRunning];
    if (delegate_ != nullptr) { ((ParticleSaturnVideoDelegate*)delegate_)->owner = nullptr; [(id)delegate_ release]; }
    if (output_ != nullptr) [(id)output_ release];
    if (session_ != nullptr) [(id)session_ release];
    session_ = output_ = delegate_ = nullptr; hasFrame_ = false;
}

bool AVFoundationCamera::IsRunning() const { std::lock_guard lock{mutex_}; return session_ != nullptr && [(AVCaptureSession*)session_ isRunning]; }
bool AVFoundationCamera::LatestFrame(Frame& frame) { std::lock_guard lock{mutex_}; if (!hasFrame_) return false; frame = latestFrame_; hasFrame_ = false; return true; }
std::string AVFoundationCamera::LastError() const { std::lock_guard lock{mutex_}; return error_; }

void AVFoundationCamera::PublishPixelBuffer(void* pixelBuffer, std::uint64_t timestampNanoseconds) {
    auto buffer = (CVPixelBufferRef)pixelBuffer;
    CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    const auto width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer));
    const auto height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer));
    const auto stride = CVPixelBufferGetBytesPerRow(buffer);
    const auto* source = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    Frame frame{width, height, timestampNanoseconds, std::vector<std::uint8_t>(width * height * 3)};
    for (std::uint32_t y = 0; y < height; ++y) for (std::uint32_t x = 0; x < width; ++x) {
        const auto* pixel = source + y * stride + x * 4;
        auto* target = frame.rgb.data() + (y * width + x) * 3;
        target[0] = pixel[2]; target[1] = pixel[1]; target[2] = pixel[0];
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    std::lock_guard lock{mutex_}; latestFrame_ = std::move(frame); hasFrame_ = true;
}

} // namespace ParticleSaturn::Services::Camera::MacOS
