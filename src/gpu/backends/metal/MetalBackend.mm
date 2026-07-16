#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "MetalBackend.h"

#include <algorithm>
#include <filesystem>

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
    [layer setFramebufferOnly:NO];
}

bool MetalSurface::AcquireDrawable() {
    drawable_ = [(CAMetalLayer*)layer_ nextDrawable];
    return drawable_ != nullptr;
}

void* MetalSurface::NativeDrawable() const noexcept {
    return drawable_;
}

bool MetalSurface::Present(MetalDevice& device) {
    if (drawable_ == nullptr) return false;
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    [commands presentDrawable:(id<CAMetalDrawable>)drawable_];
    [commands commit];
    [queue release];
    drawable_ = nullptr;
    return true;
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

id<MTLRenderPipelineState> CreateRenderPipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString* vertexName,
                                                  NSString* fragmentName) {
    id<MTLFunction> vertex = [library newFunctionWithName:vertexName];
    id<MTLFunction> fragment = [library newFunctionWithName:fragmentName];
    if (vertex == nil || fragment == nil) { [vertex release]; [fragment release]; return nil; }
    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
    descriptor.colorAttachments[0].blendingEnabled = YES;
    descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
    descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    NSError* error = nil;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    [descriptor release]; [vertex release]; [fragment release];
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

bool MetalPipelineCache::Load(MetalDevice& device, const std::string& path) {
    auto* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
    if (std::filesystem::is_regular_file(path)) {
        [descriptor setUrl:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]]];
    }
    NSError* error = nil;
    archive_ = [(id<MTLDevice>)device.NativeDevice() newBinaryArchiveWithDescriptor:descriptor error:&error];
    [descriptor release];
    return archive_ != nullptr && error == nil;
}

bool MetalPipelineCache::Save(const std::string& path) {
    if (archive_ == nullptr) return false;
    NSError* error = nil;
    const bool saved = [(id<MTLBinaryArchive>)archive_ serializeToURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]] error:&error];
    return saved && error == nil;
}

bool MetalPipelineCache::AddComputeFunction(MetalDevice& device, const std::string& libraryPath, const char* functionName) {
    NSError* error = nil;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath.c_str()]];
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice() newLibraryWithURL:url error:&error];
    if (library == nil || error != nil) return false;
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:functionName]];
    auto* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    [descriptor setComputeFunction:function];
    const bool added = [(id<MTLBinaryArchive>)archive_ addComputePipelineFunctionsWithDescriptor:descriptor error:&error];
    [descriptor release];
    [function release];
    [library release];
    return added && error == nil;
}

void* MetalPipelineCache::NativeArchive() const noexcept { return archive_; }

bool MetalStarField::Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed) {
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    NSError* error = nil;
    id<MTLLibrary> library = [nativeDevice newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> pipeline = CreateComputePipeline(library, @"InitializeStars");
    [library release];
    if (pipeline == nil) return false;
    buffer_ = [nativeDevice newBufferWithLength:StarCount * 16U options:MTLResourceStorageModePrivate];
    id<MTLCommandQueue> queue = [nativeDevice newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:(id<MTLBuffer>)buffer_ offset:0 atIndex:0];
    [encoder setBytes:&seed length:sizeof(seed) atIndex:1];
    Dispatch(encoder, pipeline, StarCount);
    [encoder endEncoding]; [commands commit]; [commands waitUntilCompleted];
    [pipeline release]; [queue release];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

void* MetalStarField::Buffer() const noexcept { return buffer_; }

bool MetalRenderTargets::Create(MetalDevice& device, std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) return false;
    auto* descriptor = [[MTLTextureDescriptor alloc] init];
    [descriptor setTextureType:MTLTextureType2D];
    [descriptor setPixelFormat:MTLPixelFormatRGBA16Float];
    [descriptor setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
    [descriptor setStorageMode:MTLStorageModePrivate];
    auto create = ^id<MTLTexture>(std::uint32_t textureWidth, std::uint32_t textureHeight) {
        [descriptor setWidth:textureWidth]; [descriptor setHeight:textureHeight];
        return [(id<MTLDevice>)device.NativeDevice() newTextureWithDescriptor:descriptor];
    };
    sceneHdr_ = create(width, height);
    bloomStrong_ = create(std::max(1U, width / 6U), std::max(1U, height / 6U));
    bloomWeak_ = create(std::max(1U, width / 12U), std::max(1U, height / 12U));
    uiOverlay_ = create(width, height);
    uiBlur_ = create(width, height);
    composite_ = create(width, height);
    [descriptor release];
    return sceneHdr_ != nullptr && bloomStrong_ != nullptr && bloomWeak_ != nullptr &&
           uiOverlay_ != nullptr && uiBlur_ != nullptr && composite_ != nullptr;
}

void* MetalRenderTargets::SceneHdr() const noexcept { return sceneHdr_; }
void* MetalRenderTargets::BloomStrong() const noexcept { return bloomStrong_; }
void* MetalRenderTargets::BloomWeak() const noexcept { return bloomWeak_; }
void* MetalRenderTargets::UiOverlay() const noexcept { return uiOverlay_; }
void* MetalRenderTargets::UiBlur() const noexcept { return uiBlur_; }
void* MetalRenderTargets::Composite() const noexcept { return composite_; }

bool MetalToneMapper::Apply(MetalDevice& device, const char* libraryPath, void* hdrTexture, void* outputTexture,
                            std::uint32_t width, std::uint32_t height) {
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> pipeline = CreateComputePipeline(library, @"ToneMap");
    [library release];
    if (pipeline == nil) return false;
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:(id<MTLTexture>)hdrTexture atIndex:0];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([pipeline threadExecutionWidth], 1, 1)];
    [encoder endEncoding]; [commands commit]; [commands waitUntilCompleted];
    [pipeline release]; [queue release];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalBloom::Apply(MetalDevice& device, const char* libraryPath, void* sceneHdr, void* strongBloom, void* weakBloom,
                       std::uint32_t strongWidth, std::uint32_t strongHeight, std::uint32_t weakWidth, std::uint32_t weakHeight) {
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice() newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> downsample = CreateComputePipeline(library, @"BloomDownsample");
    id<MTLComputePipelineState> blur = CreateComputePipeline(library, @"KawaseBlur"); [library release];
    if (downsample == nil || blur == nil) return false;
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:downsample]; [encoder setTexture:(id<MTLTexture>)sceneHdr atIndex:0]; [encoder setTexture:(id<MTLTexture>)strongBloom atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(strongWidth, strongHeight, 1) threadsPerThreadgroup:MTLSizeMake([downsample threadExecutionWidth], 1, 1)]; [encoder endEncoding];
    encoder = [commands computeCommandEncoder]; [encoder setComputePipelineState:blur]; [encoder setTexture:(id<MTLTexture>)strongBloom atIndex:0]; [encoder setTexture:(id<MTLTexture>)weakBloom atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(weakWidth, weakHeight, 1) threadsPerThreadgroup:MTLSizeMake([blur threadExecutionWidth], 1, 1)]; [encoder endEncoding];
    [commands commit]; [commands waitUntilCompleted]; [downsample release]; [blur release]; [queue release];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalAcrylic::Apply(MetalDevice& device, const char* libraryPath, void* sceneTexture, void* uiOverlayTexture,
                         void* blurredSceneTexture, void* outputTexture, std::uint32_t width, std::uint32_t height,
                         float blurRadius, float opacity) {
    if (sceneTexture == nullptr || uiOverlayTexture == nullptr || blurredSceneTexture == nullptr || outputTexture == nullptr ||
        width == 0 || height == 0 || blurRadius < 0.0f || opacity < 0.0f || opacity > 1.0f) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> blur = CreateComputePipeline(library, @"UiKawaseBlur");
    id<MTLComputePipelineState> composite = CreateComputePipeline(library, @"AcrylicComposite");
    [library release];
    if (blur == nil || composite == nil) return false;

    struct AcrylicConstants { float blurRadius; float opacity; } constants{blurRadius, opacity};
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:blur];
    [encoder setTexture:(id<MTLTexture>)sceneTexture atIndex:0];
    [encoder setTexture:(id<MTLTexture>)blurredSceneTexture atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([blur threadExecutionWidth], 1, 1)];
    [encoder endEncoding];

    encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:composite];
    [encoder setTexture:(id<MTLTexture>)sceneTexture atIndex:0];
    [encoder setTexture:(id<MTLTexture>)blurredSceneTexture atIndex:1];
    [encoder setTexture:(id<MTLTexture>)uiOverlayTexture atIndex:2];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:3];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([composite threadExecutionWidth], 1, 1)];
    [encoder endEncoding];
    [commands commit]; [commands waitUntilCompleted];
    [blur release]; [composite release]; [queue release];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalSevenSegmentFps::Render(MetalDevice& device, const char* libraryPath, void* outputTexture,
                                  std::uint32_t width, std::uint32_t height, std::uint32_t framesPerSecond) {
    if (outputTexture == nullptr || width == 0 || height == 0 || framesPerSecond > 999) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> pipeline = CreateComputePipeline(library, @"RenderSevenSegmentFps");
    [library release];
    if (pipeline == nil) return false;
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:0];
    [encoder setBytes:&framesPerSecond length:sizeof(framesPerSecond) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([pipeline threadExecutionWidth], 1, 1)];
    [encoder endEncoding]; [commands commit]; [commands waitUntilCompleted];
    [pipeline release]; [queue release];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalParticleRenderer::Initialize(MetalDevice& device, const char* libraryPath) {
    NSError* error = nil;
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    id<MTLLibrary> library = [nativeDevice newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    particlePipeline_ = CreateRenderPipeline(nativeDevice, library, @"ParticleVertex", @"ParticleFragment");
    starPipeline_ = CreateRenderPipeline(nativeDevice, library, @"StarVertex", @"StarFragment");
    [library release];
    return particlePipeline_ != nullptr && starPipeline_ != nullptr;
}

void MetalParticleRenderer::Draw(void* nativeEncoder, void* particleBuffer, void* starBuffer, std::uint32_t width,
                                 std::uint32_t height) const {
    if (nativeEncoder == nullptr || particleBuffer == nullptr || starBuffer == nullptr || height == 0) return;
    id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)nativeEncoder;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)starPipeline_];
    [encoder setVertexBuffer:(id<MTLBuffer>)starBuffer offset:0 atIndex:0];
    [encoder setVertexBytes:&aspect length:sizeof(aspect) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:MetalStarField::StarCount];
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)particlePipeline_];
    [encoder setVertexBuffer:(id<MTLBuffer>)particleBuffer offset:0 atIndex:0];
    [encoder setVertexBytes:&aspect length:sizeof(aspect) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:MetalParticleSystem::ParticleCount];
}

bool MetalFrameRenderer::Render(MetalDevice& device, MetalSurface& surface, MetalParticleSystem& particles, MetalStarField& stars,
                                MetalParticleRenderer& particleRenderer,
                                MetalRenderTargets& targets, const char* libraryPath, std::uint32_t width,
                                std::uint32_t height, float deltaTime, std::uint32_t framesPerSecond,
                                const std::function<void(void*, void*, void*)>& uiRenderer) {
    if (width == 0 || height == 0 || !surface.AcquireDrawable()) return false;
    if (!particles.Simulate(deltaTime, 1.0f, false)) return false;
    id<MTLCommandQueue> queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (id<MTLTexture>)targets.SceneHdr();
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.005, 0.008, 0.016, 1.0);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    particleRenderer.Draw(encoder, particles.RenderBuffer(), stars.Buffer(), width, height);
    [encoder endEncoding];
    [commands commit]; [commands waitUntilCompleted]; [queue release];
    if ([commands status] != MTLCommandBufferStatusCompleted) return false;
    MetalToneMapper toneMapper;
    if (!toneMapper.Apply(device, libraryPath, targets.SceneHdr(), [(id<CAMetalDrawable>)surface.NativeDrawable() texture], width, height)) return false;
    MetalSevenSegmentFps fps;
    if (!fps.Render(device, libraryPath, [(id<CAMetalDrawable>)surface.NativeDrawable() texture], width, height, framesPerSecond)) return false;
    if (uiRenderer) {
        queue = [(id<MTLDevice>)device.NativeDevice() newCommandQueue];
        commands = [queue commandBuffer];
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = [(id<CAMetalDrawable>)surface.NativeDrawable() texture];
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        uiRenderer(commands, encoder, pass);
        [encoder endEncoding]; [commands commit]; [commands waitUntilCompleted]; [queue release];
        if ([commands status] != MTLCommandBufferStatusCompleted) return false;
    }
    return surface.Present(device);
}

bool MetalIndirectDraw::Create(MetalDevice& device, std::uint32_t vertexCount) {
    struct Arguments { std::uint32_t vertexCount, instanceCount, vertexStart, baseInstance; };
    const Arguments arguments{vertexCount, 1, 0, 0};
    buffer_ = [(id<MTLDevice>)device.NativeDevice() newBufferWithBytes:&arguments length:sizeof(arguments)
        options:MTLResourceStorageModeShared];
    return buffer_ != nullptr;
}

void* MetalIndirectDraw::Buffer() const noexcept { return buffer_; }

} // namespace ParticleSaturn::Gpu::Metal
