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

namespace {

struct SimulationConstants {
    float deltaTime;
    float handScale;
    float handTracked;
    std::uint32_t particleCount;
};

constexpr std::size_t ParticleSize = 32;

id<MTLComputePipelineState> CreateComputePipeline(id<MTLLibrary> library, NSString* functionName) {
    NSError* error = nil;
    id<MTLFunction> function = [library newFunctionWithName:functionName];
    id<MTLComputePipelineState> pipeline = [(id<MTLDevice>)[library device] newComputePipelineStateWithFunction:function error:&error];
    [function release];
    return error == nil ? pipeline : nil;
}

void Dispatch(id<MTLComputeCommandEncoder> encoder, id<MTLComputePipelineState> pipeline, std::uint32_t count) {
    const NSUInteger width = [pipeline threadExecutionWidth];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
}

} // namespace

bool MetalParticleSystem::Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed) {
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    NSURL* libraryUrl = [NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]];
    id<MTLLibrary> library = [nativeDevice newLibraryWithURL:libraryUrl error:&error];
    if (library == nil) return false;
    initializePipeline_ = CreateComputePipeline(library, @"InitializeParticles");
    simulationPipeline_ = CreateComputePipeline(library, @"SimulateParticles");
    [library release];
    if (initializePipeline_ == nil || simulationPipeline_ == nil) return false;
    commandQueue_ = [nativeDevice newCommandQueue];
    for (auto& buffer : buffers_) {
        buffer = [nativeDevice newBufferWithLength:ParticleCount * ParticleSize options:MTLResourceStorageModePrivate];
        if (buffer == nil) return false;
    }
    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>)commandQueue_ commandBuffer];
    for (void* buffer : buffers_) {
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:(id<MTLComputePipelineState>)initializePipeline_];
        [encoder setBuffer:(id<MTLBuffer>)buffer offset:0 atIndex:0];
        [encoder setBytes:&seed length:sizeof(seed) atIndex:1];
        Dispatch(encoder, (id<MTLComputePipelineState>)initializePipeline_, ParticleCount);
        [encoder endEncoding];
    }
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    return [commandBuffer status] == MTLCommandBufferStatusCompleted;
}

bool MetalParticleSystem::Simulate(float deltaTime, float handScale, bool handTracked) {
    if (commandQueue_ == nil) return false;
    const SimulationConstants constants{deltaTime, handScale, handTracked ? 1.0f : 0.0f, ParticleCount};
    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>)commandQueue_ commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    [encoder setComputePipelineState:(id<MTLComputePipelineState>)simulationPipeline_];
    [encoder setBuffer:(id<MTLBuffer>)buffers_[readIndex_] offset:0 atIndex:0];
    [encoder setBuffer:(id<MTLBuffer>)buffers_[writeIndex_] offset:0 atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:2];
    Dispatch(encoder, (id<MTLComputePipelineState>)simulationPipeline_, ParticleCount);
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if ([commandBuffer status] != MTLCommandBufferStatusCompleted) return false;
    const auto previousRender = renderIndex_;
    renderIndex_ = readIndex_;
    readIndex_ = writeIndex_;
    writeIndex_ = previousRender;
    return true;
}

void* MetalParticleSystem::RenderBuffer() const noexcept { return buffers_[renderIndex_]; }

} // namespace ParticleSaturn::Gpu::Metal
