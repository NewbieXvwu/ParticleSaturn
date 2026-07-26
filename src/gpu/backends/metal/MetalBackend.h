#pragma once

#include "ParticleAbi.h"
#include "app/state/AppStates.h"
#include "gpu/interface/GpuCapabilities.h"
#include "render/ResourceRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ParticleSaturn::Gpu::Metal {

class MetalDevice {
public:
    ~MetalDevice();

    bool Initialize();
    const GpuCapabilities& Capabilities() const noexcept;
    void* NativeDevice() const noexcept;
    void* NativeCommandQueue() const noexcept;

private:
    void* device_ = nullptr;
    void* commandQueue_ = nullptr;
    GpuCapabilities capabilities_{};
};

class MetalSurface {
public:
    MetalSurface(MetalDevice& device, void* nativeLayer);
    bool AcquireDrawable();
    bool Present(MetalDevice& device);
    bool Present(void* nativeCommandBuffer);
    void* NativeDrawable() const noexcept;

private:
    MetalDevice& device_;
    void* layer_ = nullptr;
    void* drawable_ = nullptr;
};

class MetalFrameScheduler {
public:
    static constexpr std::size_t MaxFramesInFlight = 3;

    ~MetalFrameScheduler();

    std::uint64_t BeginFrame();
    void Submit(void* nativeCommandBuffer);
    void RetireResources(std::vector<void*> resources);
    bool WaitForSubmittedFrames();
    std::uint64_t LastSubmittedFrame() const noexcept;
    std::uint64_t LastCompletedFrame() const noexcept;

private:
    struct SubmittedFrame {
        std::uint64_t token = 0;
        void* commandBuffer = nullptr;
        std::vector<void*> retiredResources;
    };

    void CollectCompletedFrames();

    std::uint64_t nextFrame_ = 1;
    std::uint64_t lastSubmittedFrame_ = 0;
    std::uint64_t lastCompletedFrame_ = 0;
    std::vector<SubmittedFrame> submittedCommandBuffers_;
};

class MetalResourceManager {
public:
    explicit MetalResourceManager(MetalDevice& device);
    void* CreateBuffer(std::size_t size);

private:
    MetalDevice& device_;
};

class MetalCommandContext {
public:
    explicit MetalCommandContext(MetalDevice& device);
    bool Begin();
    void Commit();

private:
    MetalDevice& device_;
    void* commandBuffer_ = nullptr;
};

class MetalParticleSystem {
public:
    static constexpr std::uint32_t ParticleCount = 1200000;

    using ParticleSnapshot = ShaderAbi::Particle;

    bool Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed);
    bool Simulate(float deltaTime, float handScale, bool handTracked, std::uint32_t particleCount);
    bool EncodeSimulation(void* nativeCommandBuffer, float deltaTime, float handScale, bool handTracked,
                          std::uint32_t particleCount);
    bool ReadBack(std::vector<ParticleSnapshot>& particles, std::uint32_t count) const;
    void* RenderBuffer() const noexcept;

private:
    void* commandQueue_ = nullptr;
    void* initializePipeline_ = nullptr;
    void* simulationPipeline_ = nullptr;
    void* buffers_[3]{};
    std::uint32_t renderIndex_ = 0;
    std::uint32_t readIndex_ = 1;
    std::uint32_t writeIndex_ = 2;
};

class MetalPipelineCache {
public:
    ~MetalPipelineCache();

    bool Load(MetalDevice& device, const std::string& path);
    bool AddComputeFunction(MetalDevice& device, const std::string& libraryPath, const char* functionName);
    bool AddRenderFunctions(MetalDevice& device, const std::string& libraryPath, const char* vertexName,
                            const char* fragmentName);
    bool Save(const std::string& path);
    void* NativeArchive() const noexcept;

private:
    void* archive_ = nullptr;
};

class MetalStarField {
public:
    static constexpr std::uint32_t StarCount = 50000;
    bool Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed);
    void* Buffer() const noexcept;

private:
    void* buffer_ = nullptr;
};

class MetalRenderTargets {
public:
    ~MetalRenderTargets();

    bool Create(MetalDevice& device, std::uint32_t width, std::uint32_t height,
                MetalFrameScheduler* scheduler = nullptr);
    void* NativeTexture(Gpu::TextureHandle texture) const noexcept;
    Gpu::TextureHandle SceneHdrHandle() const noexcept;
    Gpu::TextureHandle BloomStrongHandle() const noexcept;
    Gpu::TextureHandle BloomPingPongHandle() const noexcept;
    Gpu::TextureHandle UiSceneHandle() const noexcept;
    Gpu::TextureHandle UiBlurHandle() const noexcept;
    Gpu::TextureHandle CompositeHandle() const noexcept;
    Gpu::TextureHandle UiBlurWeakHandle() const noexcept;
    Gpu::TextureHandle UiBlurWeakPingPongHandle() const noexcept;
    Gpu::TextureHandle UiOverlayHandle() const noexcept;
    Gpu::TextureHandle UiOverlayWeakHandle() const noexcept;
    void* SceneHdr() const noexcept;
    void* BloomStrong() const noexcept;
    void* BloomPingPong() const noexcept;
    void* BloomWeak() const noexcept;
    void* UiScene() const noexcept;
    void* UiOverlay() const noexcept;
    void* UiBlur() const noexcept;
    void* Composite() const noexcept;
    void* UiBlurWeak() const noexcept;
    void* UiBlurWeakPingPong() const noexcept;
    void* UiOverlayWeak() const noexcept;

private:
    void* sceneHdr_ = nullptr;
    void* bloomStrong_ = nullptr;
    void* bloomPingPong_ = nullptr;
    void* bloomWeak_ = nullptr;
    void* uiScene_ = nullptr;
    void* uiOverlay_ = nullptr;
    void* uiBlur_ = nullptr;
    void* composite_ = nullptr;
    void* uiBlurWeak_ = nullptr;
    void* uiBlurWeakPingPong_ = nullptr;
    void* uiOverlayWeak_ = nullptr;
    std::array<Gpu::TextureHandle, 11> handles_{};
    Render::TexturePool texturePool_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

class MetalToneMapper {
public:
    bool Apply(MetalDevice& device, const char* libraryPath, void* hdrTexture, void* bloomTexture, void* outputTexture,
               std::uint32_t width, std::uint32_t height, float bloomStrength, bool transparent = false);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* hdrTexture,
                void* bloomTexture, void* outputTexture, std::uint32_t width, std::uint32_t height,
                float bloomStrength, bool transparent = false);
};

class MetalBloom {
public:
    bool Apply(MetalDevice& device, const char* libraryPath, void* sceneHdr, void* bloomA, void* bloomB,
               std::uint32_t width, std::uint32_t height, float blurStrength);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* sceneHdr,
                void* bloomA, void* bloomB, std::uint32_t width, std::uint32_t height, float blurStrength);
};

class MetalAcrylic {
public:
    bool Apply(MetalDevice& device, const char* libraryPath, void* uiSceneTexture, void* blurA, void* blurB,
               void* blurWeakA, void* blurWeakB, void* outputTexture, void* weakOutputTexture,
               std::uint32_t width, std::uint32_t height, float blurStrength);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* uiSceneTexture,
                void* blurA, void* blurB, void* blurWeakA, void* blurWeakB, void* outputTexture,
                void* weakOutputTexture, std::uint32_t width, std::uint32_t height, float blurStrength);
};

class MetalSevenSegmentFps {
public:
    bool Render(MetalDevice& device, const char* libraryPath, void* outputTexture, std::uint32_t width,
                std::uint32_t height, std::uint32_t framesPerSecond);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* outputTexture,
                std::uint32_t width, std::uint32_t height, std::uint32_t framesPerSecond);
};

class MetalIndirectDraw {
public:
    bool Create(MetalDevice& device, std::uint32_t vertexCount);
    bool Update(std::uint32_t vertexCount);
    void* Buffer() const noexcept;
    std::uint32_t VertexCount() const noexcept;

private:
    void* buffer_ = nullptr;
    std::uint32_t vertexCount_ = 0;
};

class MetalParticleRenderer {
public:
    bool Initialize(MetalDevice& device, const char* libraryPath);
    void Draw(void* encoder, void* particleBuffer, void* starBuffer, std::uint32_t width, std::uint32_t height,
              const App::AppState& state);

private:
    void* particlePipeline_ = nullptr;
    void* starPipeline_ = nullptr;
    void* objectShaderPipeline_ = nullptr;
    MetalIndirectDraw particleIndirect_;
};

class MetalFrameRenderer {
public:
    ~MetalFrameRenderer();

    bool Render(MetalDevice& device, MetalSurface& surface, MetalParticleSystem& particles, MetalStarField& stars,
                MetalParticleRenderer& particleRenderer,
                MetalRenderTargets& targets, const char* libraryPath, std::uint32_t width, std::uint32_t height,
                float backingScale, const App::AppState& state, bool handTracked, float deltaTime,
                std::uint32_t framesPerSecond,
                const std::function<void(void*, void*, void*, void*)>& uiRenderer = {},
                const std::function<bool(void*, void*, std::uint32_t, std::uint32_t)>& sceneCapture = {});

    bool WaitForSubmittedWork();
    MetalFrameScheduler& Scheduler() noexcept;

private:
    MetalFrameScheduler scheduler_;
};

} // namespace ParticleSaturn::Gpu::Metal
