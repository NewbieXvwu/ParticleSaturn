#pragma once

#include "gpu/interface/GpuCapabilities.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace ParticleSaturn::Gpu::Metal {

class MetalDevice {
public:
    bool Initialize();
    const GpuCapabilities& Capabilities() const noexcept;
    void* NativeDevice() const noexcept;

private:
    void* device_ = nullptr;
    GpuCapabilities capabilities_{};
};

class MetalSurface {
public:
    MetalSurface(MetalDevice& device, void* nativeLayer);
    bool AcquireDrawable();
    bool Present(MetalDevice& device);
    void* NativeDrawable() const noexcept;

private:
    MetalDevice& device_;
    void* layer_ = nullptr;
    void* drawable_ = nullptr;
};

class MetalFrameScheduler {
public:
    std::uint64_t BeginFrame();
    std::uint64_t LastSubmittedFrame() const noexcept;

private:
    std::uint64_t nextFrame_ = 1;
    std::uint64_t lastSubmittedFrame_ = 0;
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

    bool Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed);
    bool Simulate(float deltaTime, float handScale, bool handTracked);
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
    bool Load(MetalDevice& device, const std::string& path);
    bool AddComputeFunction(MetalDevice& device, const std::string& libraryPath, const char* functionName);
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
    bool Create(MetalDevice& device, std::uint32_t width, std::uint32_t height);
    void* SceneHdr() const noexcept;
    void* BloomStrong() const noexcept;
    void* BloomWeak() const noexcept;
    void* UiOverlay() const noexcept;
    void* UiBlur() const noexcept;
    void* Composite() const noexcept;

private:
    void* sceneHdr_ = nullptr;
    void* bloomStrong_ = nullptr;
    void* bloomWeak_ = nullptr;
    void* uiOverlay_ = nullptr;
    void* uiBlur_ = nullptr;
    void* composite_ = nullptr;
};

class MetalToneMapper {
public:
    bool Apply(MetalDevice& device, const char* libraryPath, void* hdrTexture, void* bloomTexture, void* outputTexture,
               std::uint32_t width, std::uint32_t height);
};

class MetalBloom {
public:
    bool Apply(MetalDevice& device, const char* libraryPath, void* sceneHdr, void* strongBloom, void* weakBloom,
               std::uint32_t strongWidth, std::uint32_t strongHeight, std::uint32_t weakWidth, std::uint32_t weakHeight);
};

class MetalAcrylic {
public:
    bool BuildPanelMask(MetalDevice& device, const char* libraryPath, void* outputTexture, std::uint32_t width,
                        std::uint32_t height);
    bool Apply(MetalDevice& device, const char* libraryPath, void* sceneTexture, void* uiOverlayTexture,
               void* blurredSceneTexture, void* outputTexture, std::uint32_t width, std::uint32_t height,
               float blurRadius, float opacity);
};

class MetalSevenSegmentFps {
public:
    bool Render(MetalDevice& device, const char* libraryPath, void* outputTexture, std::uint32_t width,
                std::uint32_t height, std::uint32_t framesPerSecond);
};

class MetalParticleRenderer {
public:
    bool Initialize(MetalDevice& device, const char* libraryPath);
    void Draw(void* encoder, void* particleBuffer, void* starBuffer, std::uint32_t width, std::uint32_t height,
              float timeSeconds) const;

private:
    void* particlePipeline_ = nullptr;
    void* starPipeline_ = nullptr;
};

class MetalFrameRenderer {
public:
    bool Render(MetalDevice& device, MetalSurface& surface, MetalParticleSystem& particles, MetalStarField& stars,
                MetalParticleRenderer& particleRenderer,
                MetalRenderTargets& targets, const char* libraryPath, std::uint32_t width, std::uint32_t height,
                float deltaTime, std::uint32_t framesPerSecond,
                const std::function<void(void*, void*, void*)>& uiRenderer = {});

private:
    float elapsedTime_ = 0.0f;
};

class MetalIndirectDraw {
public:
    bool Create(MetalDevice& device, std::uint32_t vertexCount);
    void* Buffer() const noexcept;

private:
    void* buffer_ = nullptr;
};

} // namespace ParticleSaturn::Gpu::Metal
