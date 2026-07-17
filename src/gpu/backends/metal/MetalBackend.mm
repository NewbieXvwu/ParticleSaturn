#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "MetalBackend.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace ParticleSaturn::Gpu::Metal {

bool MetalDevice::Initialize() {
    device_ = MTLCreateSystemDefaultDevice();
    if (device_ == nullptr) {
        return false;
    }
    commandQueue_ = [(id<MTLDevice>)device_ newCommandQueue];
    if (commandQueue_ == nullptr) return false;
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

MetalDevice::~MetalDevice() {
    [(id<MTLCommandQueue>)commandQueue_ release];
    [(id<MTLDevice>)device_ release];
}

const GpuCapabilities& MetalDevice::Capabilities() const noexcept {
    return capabilities_;
}

void* MetalDevice::NativeDevice() const noexcept {
    return device_;
}

void* MetalDevice::NativeCommandQueue() const noexcept {
    return commandQueue_;
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
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (!Present(commands)) return false;
    [commands commit];
    return true;
}

bool MetalSurface::Present(void* nativeCommandBuffer) {
    if (drawable_ == nullptr || nativeCommandBuffer == nullptr) return false;
    [(id<MTLCommandBuffer>)nativeCommandBuffer presentDrawable:(id<CAMetalDrawable>)drawable_];
    drawable_ = nullptr;
    return true;
}

std::uint64_t MetalFrameScheduler::BeginFrame() {
    CollectCompletedFrames();
    while (submittedCommandBuffers_.size() >= MaxFramesInFlight) {
        id<MTLCommandBuffer> oldest = (id<MTLCommandBuffer>)submittedCommandBuffers_.front();
        [oldest waitUntilCompleted];
        [oldest release];
        submittedCommandBuffers_.erase(submittedCommandBuffers_.begin());
        CollectCompletedFrames();
    }
    lastSubmittedFrame_ = nextFrame_++;
    return lastSubmittedFrame_;
}

void MetalFrameScheduler::Submit(void* nativeCommandBuffer) {
    if (nativeCommandBuffer == nullptr) return;
    [(id<MTLCommandBuffer>)nativeCommandBuffer retain];
    submittedCommandBuffers_.push_back(nativeCommandBuffer);
}

void MetalFrameScheduler::RetireResources(std::vector<void*> resources) {
    if (resources.empty()) return;
    NSMutableArray* retired = [[NSMutableArray alloc] initWithCapacity:resources.size()];
    for (void* resource : resources) {
        if (resource != nullptr) [retired addObject:(id)resource];
    }
    if ([retired count] == 0) {
        [retired release];
        return;
    }
    if (submittedCommandBuffers_.empty() ||
        [(id<MTLCommandBuffer>)submittedCommandBuffers_.back() status] >= MTLCommandBufferStatusCompleted) {
        [retired release];
        return;
    }
    [(id<MTLCommandBuffer>)submittedCommandBuffers_.back() addCompletedHandler:^(id<MTLCommandBuffer>) {
        [retired release];
    }];
}

void MetalFrameScheduler::CollectCompletedFrames() {
    auto next = submittedCommandBuffers_.begin();
    for (auto current = submittedCommandBuffers_.begin(); current != submittedCommandBuffers_.end(); ++current) {
        const auto status = [(id<MTLCommandBuffer>)*current status];
        if (status < MTLCommandBufferStatusCompleted) {
            *next++ = *current;
            continue;
        }
        [(id<MTLCommandBuffer>)*current release];
    }
    submittedCommandBuffers_.erase(next, submittedCommandBuffers_.end());
}

bool MetalFrameScheduler::WaitForSubmittedFrames() {
    bool completed = true;
    for (void* commandBuffer : submittedCommandBuffers_) {
        [(id<MTLCommandBuffer>)commandBuffer waitUntilCompleted];
        completed = completed && [(id<MTLCommandBuffer>)commandBuffer status] == MTLCommandBufferStatusCompleted;
        [(id<MTLCommandBuffer>)commandBuffer release];
    }
    submittedCommandBuffers_.clear();
    return completed;
}

MetalFrameScheduler::~MetalFrameScheduler() {
    WaitForSubmittedFrames();
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
    commandBuffer_ = [(id<MTLCommandQueue>)device_.NativeCommandQueue() commandBuffer];
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
id<MTLBinaryArchive> ActivePipelineArchive = nil;

id<MTLComputePipelineState> CreateComputePipeline(id<MTLLibrary> library, NSString* functionName) {
    NSError* error = nil;
    id<MTLFunction> function = [library newFunctionWithName:functionName];
    auto* descriptor = [[MTLComputePipelineDescriptor alloc] init];
    descriptor.computeFunction = function;
    if (ActivePipelineArchive != nil) descriptor.binaryArchives = @[ActivePipelineArchive];
    id<MTLComputePipelineState> pipeline = [(id<MTLDevice>)[library device] newComputePipelineStateWithDescriptor:descriptor options:0 reflection:nil error:&error];
    [descriptor release];
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
    commandQueue_ = device.NativeCommandQueue();
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

bool MetalParticleSystem::Simulate(float deltaTime, float handScale, bool handTracked, std::uint32_t particleCount) {
    if (commandQueue_ == nil) return false;
    id<MTLCommandBuffer> commandBuffer = [(id<MTLCommandQueue>)commandQueue_ commandBuffer];
    const auto renderIndex = renderIndex_;
    const auto readIndex = readIndex_;
    const auto writeIndex = writeIndex_;
    if (!EncodeSimulation(commandBuffer, deltaTime, handScale, handTracked, particleCount)) return false;
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if ([commandBuffer status] == MTLCommandBufferStatusCompleted) return true;
    renderIndex_ = renderIndex;
    readIndex_ = readIndex;
    writeIndex_ = writeIndex;
    return false;
}

bool MetalParticleSystem::EncodeSimulation(void* nativeCommandBuffer, float deltaTime, float handScale, bool handTracked,
                                           std::uint32_t particleCount) {
    if (nativeCommandBuffer == nullptr || commandQueue_ == nil) return false;
    const SimulationConstants constants{deltaTime, handScale, handTracked ? 1.0f : 0.0f,
                                        std::clamp(particleCount, 1U, ParticleCount)};
    id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)nativeCommandBuffer;
    id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
    [encoder setComputePipelineState:(id<MTLComputePipelineState>)simulationPipeline_];
    [encoder setBuffer:(id<MTLBuffer>)buffers_[readIndex_] offset:0 atIndex:0];
    [encoder setBuffer:(id<MTLBuffer>)buffers_[writeIndex_] offset:0 atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:2];
    Dispatch(encoder, (id<MTLComputePipelineState>)simulationPipeline_, ParticleCount);
    [encoder endEncoding];
    const auto previousRender = renderIndex_;
    renderIndex_ = readIndex_;
    readIndex_ = writeIndex_;
    writeIndex_ = previousRender;
    return true;
}

bool MetalParticleSystem::ReadBack(std::vector<ParticleSnapshot>& particles, std::uint32_t count) const {
    if (commandQueue_ == nullptr || buffers_[renderIndex_] == nullptr || count == 0 || count > ParticleCount) return false;
    id<MTLBuffer> source = (id<MTLBuffer>)buffers_[renderIndex_];
    id<MTLDevice> device = [source device];
    const NSUInteger length = static_cast<NSUInteger>(count) * sizeof(ParticleSnapshot);
    id<MTLBuffer> staging = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (staging == nil) return false;
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)commandQueue_ commandBuffer];
    id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
    [encoder copyFromBuffer:source sourceOffset:0 toBuffer:staging destinationOffset:0 size:length];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    const bool completed = [commands status] == MTLCommandBufferStatusCompleted;
    if (completed) {
        particles.resize(count);
        std::memcpy(particles.data(), [staging contents], length);
    }
    [staging release];
    return completed;
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
    ActivePipelineArchive = (id<MTLBinaryArchive>)archive_;
    return archive_ != nullptr && error == nil;
}

MetalPipelineCache::~MetalPipelineCache() {
    if (ActivePipelineArchive == archive_) ActivePipelineArchive = nil;
    [(id)archive_ release];
    archive_ = nullptr;
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
    (void)libraryPath;
    (void)seed;
    struct Star { float position[3]; float color[3]; float size; float randomSeed; };
    std::vector<Star> stars(StarCount);
    std::mt19937 generator{1337U};
    std::uniform_real_distribution<float> random{0.0f, 1.0f};
    constexpr float colors[4][3] = {{0.890f, 0.855f, 0.773f}, {0.788f, 0.627f, 0.439f},
                                    {0.890f, 0.855f, 0.773f}, {0.690f, 0.553f, 0.333f}};
    for (std::uint32_t index = 0; index < StarCount; ++index) {
        const float radius = 400.0f + random(generator) * 3000.0f;
        const float theta = random(generator) * 6.28318530718f;
        const float phi = std::acos(2.0f * random(generator) - 1.0f);
        auto& star = stars[index];
        star.position[0] = radius * std::sin(phi) * std::cos(theta);
        star.position[1] = radius * std::cos(phi);
        star.position[2] = radius * std::sin(phi) * std::sin(theta);
        star.color[0] = colors[index % 4][0]; star.color[1] = colors[index % 4][1]; star.color[2] = colors[index % 4][2];
        star.size = 1.0f + random(generator) * 3.0f;
        star.randomSeed = random(generator);
    }
    buffer_ = [nativeDevice newBufferWithBytes:stars.data() length:stars.size() * sizeof(Star) options:MTLResourceStorageModeShared];
    return buffer_ != nullptr;
}

void* MetalStarField::Buffer() const noexcept { return buffer_; }

bool MetalRenderTargets::Create(MetalDevice& device, std::uint32_t width, std::uint32_t height,
                                MetalFrameScheduler* scheduler) {
    if (width == 0 || height == 0) return false;
    auto releaseTextures = [](std::array<void*, 11>& textures) {
        for (void*& texture : textures) {
            if (texture != nullptr) {
                [(id<MTLTexture>)texture release];
                texture = nullptr;
            }
        }
    };
    auto* descriptor = [[MTLTextureDescriptor alloc] init];
    [descriptor setTextureType:MTLTextureType2D];
    [descriptor setPixelFormat:MTLPixelFormatRGBA16Float];
    [descriptor setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite];
    [descriptor setStorageMode:MTLStorageModePrivate];
    auto create = ^id<MTLTexture>(std::uint32_t textureWidth, std::uint32_t textureHeight) {
        [descriptor setWidth:textureWidth]; [descriptor setHeight:textureHeight];
        return [(id<MTLDevice>)device.NativeDevice() newTextureWithDescriptor:descriptor];
    };
    const std::uint32_t strongWidth = std::max(1U, width / 6U);
    const std::uint32_t strongHeight = std::max(1U, height / 6U);
    const std::uint32_t weakWidth = std::max(1U, width / 12U);
    const std::uint32_t weakHeight = std::max(1U, height / 12U);
    std::array<void*, 11> replacements{
        create(width, height), create(strongWidth, strongHeight), create(strongWidth, strongHeight),
        create(weakWidth, weakHeight), create(width, height), create(strongWidth, strongHeight),
        create(strongWidth, strongHeight), create(strongWidth, strongHeight), create(weakWidth, weakHeight),
        create(weakWidth, weakHeight), create(weakWidth, weakHeight)};
    [descriptor release];
    if (std::any_of(replacements.begin(), replacements.end(), [](void* texture) { return texture == nullptr; })) {
        releaseTextures(replacements);
        return false;
    }
    std::array<void*, 11> previous{sceneHdr_, bloomStrong_, bloomPingPong_, bloomWeak_, uiScene_, uiOverlay_, uiBlur_,
                                   composite_, uiBlurWeak_, uiBlurWeakPingPong_, uiOverlayWeak_};
    sceneHdr_ = replacements[0]; bloomStrong_ = replacements[1]; bloomPingPong_ = replacements[2];
    bloomWeak_ = replacements[3]; uiScene_ = replacements[4]; uiOverlay_ = replacements[5];
    uiBlur_ = replacements[6]; composite_ = replacements[7]; uiBlurWeak_ = replacements[8];
    uiBlurWeakPingPong_ = replacements[9]; uiOverlayWeak_ = replacements[10];
    if (scheduler != nullptr) {
        scheduler->RetireResources({previous.begin(), previous.end()});
    } else {
        releaseTextures(previous);
    }
    return true;
}

MetalRenderTargets::~MetalRenderTargets() {
    const auto releaseTexture = [](void*& texture) {
        [(id<MTLTexture>)texture release];
        texture = nullptr;
    };
    releaseTexture(sceneHdr_); releaseTexture(bloomStrong_); releaseTexture(bloomPingPong_); releaseTexture(bloomWeak_);
    releaseTexture(uiScene_); releaseTexture(uiOverlay_); releaseTexture(uiBlur_); releaseTexture(composite_);
    releaseTexture(uiBlurWeak_); releaseTexture(uiBlurWeakPingPong_); releaseTexture(uiOverlayWeak_);
}

void* MetalRenderTargets::SceneHdr() const noexcept { return sceneHdr_; }
void* MetalRenderTargets::BloomStrong() const noexcept { return bloomStrong_; }
void* MetalRenderTargets::BloomPingPong() const noexcept { return bloomPingPong_; }
void* MetalRenderTargets::BloomWeak() const noexcept { return bloomWeak_; }
void* MetalRenderTargets::UiScene() const noexcept { return uiScene_; }
void* MetalRenderTargets::UiOverlay() const noexcept { return uiOverlay_; }
void* MetalRenderTargets::UiBlur() const noexcept { return uiBlur_; }
void* MetalRenderTargets::Composite() const noexcept { return composite_; }
void* MetalRenderTargets::UiBlurWeak() const noexcept { return uiBlurWeak_; }
void* MetalRenderTargets::UiBlurWeakPingPong() const noexcept { return uiBlurWeakPingPong_; }
void* MetalRenderTargets::UiOverlayWeak() const noexcept { return uiOverlayWeak_; }

bool MetalToneMapper::Apply(MetalDevice& device, const char* libraryPath, void* hdrTexture, void* bloomTexture,
                            void* outputTexture, std::uint32_t width, std::uint32_t height, float bloomStrength,
                            bool transparent) {
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (!Encode(device, commands, libraryPath, hdrTexture, bloomTexture, outputTexture, width, height, bloomStrength, transparent)) return false;
    [commands commit]; [commands waitUntilCompleted];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalToneMapper::Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* hdrTexture,
                              void* bloomTexture, void* outputTexture, std::uint32_t width, std::uint32_t height,
                              float bloomStrength, bool transparent) {
    if (nativeCommandBuffer == nullptr || width == 0 || height == 0) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    if (hdrTexture == nullptr || bloomTexture == nullptr || outputTexture == nullptr) { [library release]; return false; }
    id<MTLComputePipelineState> pipeline = CreateComputePipeline(library, @"ToneMapWithBloom");
    [library release];
    if (pipeline == nil) return false;
    id<MTLCommandBuffer> commands = (id<MTLCommandBuffer>)nativeCommandBuffer;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:(id<MTLTexture>)hdrTexture atIndex:0];
    [encoder setTexture:(id<MTLTexture>)bloomTexture atIndex:1];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:2];
    const float clampedBloomStrength = std::max(0.0f, bloomStrength);
    [encoder setBytes:&clampedBloomStrength length:sizeof(clampedBloomStrength) atIndex:0];
    const float transparentValue = transparent ? 1.0f : 0.0f;
    [encoder setBytes:&transparentValue length:sizeof(transparentValue) atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([pipeline threadExecutionWidth], 1, 1)];
    [encoder endEncoding];
    [pipeline release];
    return true;
}

bool MetalBloom::Apply(MetalDevice& device, const char* libraryPath, void* sceneHdr, void* bloomA, void* bloomB,
                       std::uint32_t width, std::uint32_t height, float blurStrength) {
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (!Encode(device, commands, libraryPath, sceneHdr, bloomA, bloomB, width, height, blurStrength)) return false;
    [commands commit]; [commands waitUntilCompleted];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalBloom::Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* sceneHdr,
                        void* bloomA, void* bloomB, std::uint32_t width, std::uint32_t height, float blurStrength) {
    if (nativeCommandBuffer == nullptr || sceneHdr == nullptr || bloomA == nullptr || bloomB == nullptr || width == 0 || height == 0) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice() newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> downsample = CreateComputePipeline(library, @"BloomDownsample");
    id<MTLComputePipelineState> blur = CreateComputePipeline(library, @"KawaseBlur"); [library release];
    if (downsample == nil || blur == nil) return false;
    const std::uint32_t bloomWidth = std::max(1U, width / 6U);
    const std::uint32_t bloomHeight = std::max(1U, height / 6U);
    // The bright-pass reads the full-size HDR scene.  Its texel size must
    // therefore describe that source, matching Diligent's RenderBloom pass.
    struct BloomConstants { float texelX, texelY, offset, threshold; } constants{
        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 1.0f};
    id<MTLCommandBuffer> commands = (id<MTLCommandBuffer>)nativeCommandBuffer;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:downsample]; [encoder setTexture:(id<MTLTexture>)sceneHdr atIndex:0]; [encoder setTexture:(id<MTLTexture>)bloomA atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(bloomWidth, bloomHeight, 1) threadsPerThreadgroup:MTLSizeMake([downsample threadExecutionWidth], 1, 1)]; [encoder endEncoding];

    static constexpr float offsets[] = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float scale = std::clamp(blurStrength, 0.0f, 5.0f) / 5.0f;
    for (std::size_t index = 1; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        constants.offset = scale * (offsets[index] + 0.5f) - 0.5f;
        const bool writeToB = (index % 2U) == 1U;
        constants.texelX = 1.0f / static_cast<float>(bloomWidth);
        constants.texelY = 1.0f / static_cast<float>(bloomHeight);
        encoder = [commands computeCommandEncoder]; [encoder setComputePipelineState:blur];
        [encoder setTexture:(id<MTLTexture>)(writeToB ? bloomA : bloomB) atIndex:0];
        [encoder setTexture:(id<MTLTexture>)(writeToB ? bloomB : bloomA) atIndex:1];
        [encoder setBytes:&constants length:sizeof(constants) atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(bloomWidth, bloomHeight, 1) threadsPerThreadgroup:MTLSizeMake([blur threadExecutionWidth], 1, 1)]; [encoder endEncoding];
    }
    [downsample release]; [blur release];
    return true;
}

bool MetalAcrylic::Apply(MetalDevice& device, const char* libraryPath, void* uiSceneTexture, void* blurA, void* blurB,
                         void* blurWeakA, void* blurWeakB, void* outputTexture, void* weakOutputTexture,
                         std::uint32_t width, std::uint32_t height, float blurStrength) {
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (!Encode(device, commands, libraryPath, uiSceneTexture, blurA, blurB, blurWeakA, blurWeakB, outputTexture,
                weakOutputTexture, width, height, blurStrength)) return false;
    [commands commit]; [commands waitUntilCompleted];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalAcrylic::Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath,
                          void* uiSceneTexture, void* blurA, void* blurB, void* blurWeakA, void* blurWeakB,
                          void* outputTexture, void* weakOutputTexture, std::uint32_t width, std::uint32_t height,
                          float blurStrength) {
    if (uiSceneTexture == nullptr || blurA == nullptr || blurB == nullptr || blurWeakA == nullptr ||
        blurWeakB == nullptr || outputTexture == nullptr || weakOutputTexture == nullptr ||
        nativeCommandBuffer == nullptr || width == 0 || height == 0 || blurStrength < 0.0f) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> downsample = CreateComputePipeline(library, @"BloomDownsample");
    id<MTLComputePipelineState> blur = CreateComputePipeline(library, @"KawaseBlur");
    id<MTLComputePipelineState> composite = CreateComputePipeline(library, @"AcrylicComposite");
    [library release];
    if (downsample == nil || blur == nil || composite == nil) return false;

    const std::uint32_t blurWidth = std::max(1U, width / 6U);
    const std::uint32_t blurHeight = std::max(1U, height / 6U);
    struct BloomConstants { float texelX, texelY, offset, threshold; } blurConstants{
        1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), 0.0f, 0.0f};
    id<MTLCommandBuffer> commands = (id<MTLCommandBuffer>)nativeCommandBuffer;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:downsample];
    [encoder setTexture:(id<MTLTexture>)uiSceneTexture atIndex:0];
    [encoder setTexture:(id<MTLTexture>)blurA atIndex:1];
    [encoder setBytes:&blurConstants length:sizeof(blurConstants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(blurWidth, blurHeight, 1)
      threadsPerThreadgroup:MTLSizeMake([downsample threadExecutionWidth], 1, 1)];
    [encoder endEncoding];

    static constexpr float offsets[] = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    const float scale = std::clamp(blurStrength, 0.0f, 5.0f) / 5.0f;
    for (std::size_t index = 1; index < sizeof(offsets) / sizeof(offsets[0]); ++index) {
        blurConstants.texelX = 1.0f / static_cast<float>(blurWidth);
        blurConstants.texelY = 1.0f / static_cast<float>(blurHeight);
        blurConstants.offset = scale * (offsets[index] + 0.5f) - 0.5f;
        const bool writeToB = (index % 2U) == 1U;
        encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:blur];
        [encoder setTexture:(id<MTLTexture>)(writeToB ? blurA : blurB) atIndex:0];
        [encoder setTexture:(id<MTLTexture>)(writeToB ? blurB : blurA) atIndex:1];
        [encoder setBytes:&blurConstants length:sizeof(blurConstants) atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(blurWidth, blurHeight, 1)
          threadsPerThreadgroup:MTLSizeMake([blur threadExecutionWidth], 1, 1)];
        [encoder endEncoding];
    }

    // Diligent renders a second, weaker Acrylic source at 1/12 resolution.
    // It starts from the completed 1/6 blur and returns to blurWeakA after
    // two low-offset Kawase passes (C -> D -> C in the original path).
    const std::uint32_t weakWidth = std::max(1U, width / 12U);
    const std::uint32_t weakHeight = std::max(1U, height / 12U);
    blurConstants.texelX = 1.0f / static_cast<float>(blurWidth);
    blurConstants.texelY = 1.0f / static_cast<float>(blurHeight);
    blurConstants.offset = 0.0f;
    blurConstants.threshold = 0.0f;
    encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:downsample];
    [encoder setTexture:(id<MTLTexture>)blurB atIndex:0];
    [encoder setTexture:(id<MTLTexture>)blurWeakA atIndex:1];
    [encoder setBytes:&blurConstants length:sizeof(blurConstants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(weakWidth, weakHeight, 1)
      threadsPerThreadgroup:MTLSizeMake([downsample threadExecutionWidth], 1, 1)];
    [encoder endEncoding];

    static constexpr float weakOffsets[] = {0.5f, 1.0f};
    for (std::size_t index = 0; index < sizeof(weakOffsets) / sizeof(weakOffsets[0]); ++index) {
        blurConstants.texelX = 1.0f / static_cast<float>(weakWidth);
        blurConstants.texelY = 1.0f / static_cast<float>(weakHeight);
        blurConstants.offset = scale * (weakOffsets[index] + 0.5f) - 0.5f;
        const bool writeToWeakB = index == 0U;
        encoder = [commands computeCommandEncoder];
        [encoder setComputePipelineState:blur];
        [encoder setTexture:(id<MTLTexture>)(writeToWeakB ? blurWeakA : blurWeakB) atIndex:0];
        [encoder setTexture:(id<MTLTexture>)(writeToWeakB ? blurWeakB : blurWeakA) atIndex:1];
        [encoder setBytes:&blurConstants length:sizeof(blurConstants) atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(weakWidth, weakHeight, 1)
          threadsPerThreadgroup:MTLSizeMake([blur threadExecutionWidth], 1, 1)];
        [encoder endEncoding];
    }

    struct AcrylicConstants { float tintR, tintG, tintB, baseOpacity, saturation, adaptive, darkMode, exclusion; } constants{
        20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 180.0f / 255.0f, 1.35f, 0.35f, 1.0f, 1.0f};
    encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:composite];
    [encoder setTexture:(id<MTLTexture>)blurB atIndex:0];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(blurWidth, blurHeight, 1)
      threadsPerThreadgroup:MTLSizeMake([composite threadExecutionWidth], 1, 1)];
    [encoder endEncoding];

    const AcrylicConstants weakConstants{
        35.0f / 255.0f, 35.0f / 255.0f, 40.0f / 255.0f, 160.0f / 255.0f, 1.30f, 0.30f, 1.0f, 1.0f};
    encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:composite];
    [encoder setTexture:(id<MTLTexture>)blurWeakA atIndex:0];
    [encoder setTexture:(id<MTLTexture>)weakOutputTexture atIndex:1];
    [encoder setBytes:&weakConstants length:sizeof(weakConstants) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(weakWidth, weakHeight, 1)
      threadsPerThreadgroup:MTLSizeMake([composite threadExecutionWidth], 1, 1)];
    [encoder endEncoding];
    [downsample release]; [blur release]; [composite release];
    return true;
}

bool MetalSevenSegmentFps::Render(MetalDevice& device, const char* libraryPath, void* outputTexture,
                                  std::uint32_t width, std::uint32_t height, std::uint32_t framesPerSecond) {
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (!Encode(device, commands, libraryPath, outputTexture, width, height, framesPerSecond)) return false;
    [commands commit]; [commands waitUntilCompleted];
    return [commands status] == MTLCommandBufferStatusCompleted;
}

bool MetalSevenSegmentFps::Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath,
                                  void* outputTexture, std::uint32_t width, std::uint32_t height,
                                  std::uint32_t framesPerSecond) {
    if (nativeCommandBuffer == nullptr || outputTexture == nullptr || width == 0 || height == 0 || framesPerSecond > 999) return false;
    NSError* error = nil;
    id<MTLLibrary> library = [(id<MTLDevice>)device.NativeDevice()
        newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    id<MTLComputePipelineState> pipeline = CreateComputePipeline(library, @"RenderSevenSegmentFps");
    [library release];
    if (pipeline == nil) return false;
    id<MTLCommandBuffer> commands = (id<MTLCommandBuffer>)nativeCommandBuffer;
    id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:(id<MTLTexture>)outputTexture atIndex:0];
    [encoder setBytes:&framesPerSecond length:sizeof(framesPerSecond) atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width, height, 1)
      threadsPerThreadgroup:MTLSizeMake([pipeline threadExecutionWidth], 1, 1)];
    [encoder endEncoding];
    [pipeline release];
    return true;
}

bool MetalParticleRenderer::Initialize(MetalDevice& device, const char* libraryPath) {
    NSError* error = nil;
    id<MTLDevice> nativeDevice = (id<MTLDevice>)device.NativeDevice();
    id<MTLLibrary> library = [nativeDevice newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:libraryPath]] error:&error];
    if (library == nil || error != nil) return false;
    particlePipeline_ = CreateRenderPipeline(nativeDevice, library, @"ParticleVertex", @"ParticleFragment");
    starPipeline_ = CreateRenderPipeline(nativeDevice, library, @"StarVertex", @"StarFragment");
    [library release];
    return particlePipeline_ != nullptr && starPipeline_ != nullptr &&
           particleIndirect_.Create(device, MetalParticleSystem::ParticleCount);
}

void MetalParticleRenderer::Draw(void* nativeEncoder, void* particleBuffer, void* starBuffer, std::uint32_t width,
                                 std::uint32_t height, const App::AppState& state) {
    if (nativeEncoder == nullptr || particleBuffer == nullptr || starBuffer == nullptr || height == 0) return;
    id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)nativeEncoder;
    struct RenderConstants { float aspect, screenHeight, time, scale, rotationX, rotationY, pixelRatio, densityCompensation; } constants{
        static_cast<float>(width) / static_cast<float>(height), static_cast<float>(height),
        static_cast<float>(state.scene.simulationTimeSeconds), state.scene.zoom, state.scene.rotationX, state.scene.rotationY,
        state.render.pixelRatio, state.render.densityCompensation};
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)starPipeline_];
    [encoder setVertexBuffer:(id<MTLBuffer>)starBuffer offset:0 atIndex:0];
    [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:MetalStarField::StarCount];
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)particlePipeline_];
    [encoder setVertexBuffer:(id<MTLBuffer>)particleBuffer offset:0 atIndex:0];
    [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
    const auto particleCount = std::clamp(state.render.particleCount, 1U, MetalParticleSystem::ParticleCount);
    if (!particleIndirect_.Update(particleCount)) return;
    [encoder drawPrimitives:MTLPrimitiveTypePoint
             indirectBuffer:(id<MTLBuffer>)particleIndirect_.Buffer()
       indirectBufferOffset:0];
}

MetalFrameRenderer::~MetalFrameRenderer() {
    WaitForSubmittedWork();
}

bool MetalFrameRenderer::WaitForSubmittedWork() {
    return scheduler_.WaitForSubmittedFrames();
}

MetalFrameScheduler& MetalFrameRenderer::Scheduler() noexcept {
    return scheduler_;
}

bool MetalFrameRenderer::Render(MetalDevice& device, MetalSurface& surface, MetalParticleSystem& particles, MetalStarField& stars,
                                MetalParticleRenderer& particleRenderer,
                                MetalRenderTargets& targets, const char* libraryPath, std::uint32_t width,
                                std::uint32_t height, float backingScale, const App::AppState& state, bool handTracked,
                                float deltaTime, std::uint32_t framesPerSecond,
                                const std::function<void(void*, void*, void*)>& uiRenderer,
                                const std::function<bool(void*, void*, std::uint32_t, std::uint32_t)>& sceneCapture) {
    if (width == 0 || height == 0 || !surface.AcquireDrawable()) return false;
    scheduler_.BeginFrame();
    (void)backingScale;
    id<MTLCommandBuffer> commands = [(id<MTLCommandQueue>)device.NativeCommandQueue() commandBuffer];
    if (commands == nil) return false;
    if (!state.scene.paused &&
        !particles.EncodeSimulation(commands, deltaTime, state.scene.zoom, handTracked, state.render.particleCount)) return false;
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (id<MTLTexture>)targets.SceneHdr();
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.005, 0.008, 0.016, 1.0);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    particleRenderer.Draw(encoder, particles.RenderBuffer(), stars.Buffer(), width, height, state);
    [encoder endEncoding];
    MetalBloom bloom;
    if (!bloom.Encode(device, commands, libraryPath, targets.SceneHdr(), targets.BloomStrong(), targets.BloomPingPong(),
                      width, height, state.render.bloomBlurStrength)) return false;
    MetalToneMapper toneMapper;
    const float bloomStrength = state.render.bloomEnabled ? 0.5f : 0.0f;
    id<MTLTexture> drawableTexture = [(id<CAMetalDrawable>)surface.NativeDrawable() texture];
    if (!toneMapper.Encode(device, commands, libraryPath, targets.SceneHdr(), targets.BloomPingPong(), drawableTexture,
                           width, height, bloomStrength, state.window.material == App::WindowMaterial::SystemBlur)) return false;
    if (state.ui.blurEnabled) {
        if (!toneMapper.Encode(device, commands, libraryPath, targets.SceneHdr(), targets.BloomPingPong(), targets.UiScene(),
                               width, height, bloomStrength)) return false;
        MetalAcrylic acrylic;
        if (!acrylic.Encode(device, commands, libraryPath, targets.UiScene(), targets.UiBlur(), targets.Composite(),
                            targets.UiBlurWeak(), targets.UiBlurWeakPingPong(), targets.UiOverlay(), targets.UiOverlayWeak(),
                            width, height, state.ui.blurStrength)) return false;
    }
    MetalSevenSegmentFps fps;
    if (!fps.Encode(device, commands, libraryPath, drawableTexture, width, height, framesPerSecond)) return false;
    if (uiRenderer) {
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawableTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        uiRenderer(commands, encoder, pass);
        [encoder endEncoding];
    }
    if (!surface.Present(commands)) return false;
    [commands commit];
    scheduler_.Submit(commands);
    if (!sceneCapture) return true;
    [commands waitUntilCompleted];
    return [commands status] == MTLCommandBufferStatusCompleted && sceneCapture(device.NativeDevice(), drawableTexture, width, height);
}

bool MetalIndirectDraw::Create(MetalDevice& device, std::uint32_t vertexCount) {
    struct Arguments { std::uint32_t vertexCount, instanceCount, vertexStart, baseInstance; };
    const Arguments arguments{vertexCount, 1, 0, 0};
    buffer_ = [(id<MTLDevice>)device.NativeDevice() newBufferWithBytes:&arguments length:sizeof(arguments)
        options:MTLResourceStorageModeShared];
    vertexCount_ = buffer_ == nullptr ? 0U : vertexCount;
    return buffer_ != nullptr;
}

bool MetalIndirectDraw::Update(std::uint32_t vertexCount) {
    if (buffer_ == nullptr || vertexCount == 0U) return false;
    struct Arguments { std::uint32_t vertexCount, instanceCount, vertexStart, baseInstance; };
    auto* arguments = static_cast<Arguments*>([(id<MTLBuffer>)buffer_ contents]);
    if (arguments == nullptr) return false;
    *arguments = {vertexCount, 1U, 0U, 0U};
    vertexCount_ = vertexCount;
    return true;
}

void* MetalIndirectDraw::Buffer() const noexcept { return buffer_; }
std::uint32_t MetalIndirectDraw::VertexCount() const noexcept { return vertexCount_; }

} // namespace ParticleSaturn::Gpu::Metal
