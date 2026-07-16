#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "MetalBackend.h"

namespace ParticleSaturn::Gpu::Metal {

bool MetalDevice::Initialize() {
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nullptr) {
        return false;
    }
    capabilities_ = {
        true,
        true,
        true,
        false,
        true,
        true,
        true,
        false,
    };
    return true;
}

const GpuCapabilities& MetalDevice::Capabilities() const noexcept {
    return capabilities_;
}

void* MetalDevice::NativeDevice() const noexcept {
    return device_;
}

MetalSurface::MetalSurface(MetalDevice& device, void* nativeLayer) : device_{device}, layer_{nativeLayer} {
    auto* layer = (CAMetalLayer*)layer_;
    [layer setDevice:(id<MTLDevice>)device_.NativeDevice()];
    [layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
    [layer setFramebufferOnly:YES];
}

bool MetalSurface::AcquireDrawable() {
    drawable_ = [(CAMetalLayer*)layer_ nextDrawable];
    return drawable_ != nullptr;
}

void* MetalSurface::NativeDrawable() const noexcept {
    return drawable_;
}

std::uint64_t MetalFrameScheduler::BeginFrame() {
    lastSubmittedFrame_ = nextFrame_++;
    return lastSubmittedFrame_;
}

std::uint64_t MetalFrameScheduler::LastSubmittedFrame() const noexcept {
    return lastSubmittedFrame_;
}

MetalResourceManager::MetalResourceManager(MetalDevice& device) : device_{device} {}

void* MetalResourceManager::CreateBuffer(std::size_t size) {
    return [(id<MTLDevice>)device_.NativeDevice() newBufferWithLength:size options:MTLResourceStorageModePrivate];
}

MetalCommandContext::MetalCommandContext(MetalDevice& device) : device_{device} {}

bool MetalCommandContext::Begin() {
    commandBuffer_ = [[(id<MTLDevice>)device_.NativeDevice() newCommandQueue] commandBuffer];
    return commandBuffer_ != nullptr;
}

void MetalCommandContext::Commit() {
    [(id<MTLCommandBuffer>)commandBuffer_ commit];
    commandBuffer_ = nullptr;
}

} // namespace ParticleSaturn::Gpu::Metal
