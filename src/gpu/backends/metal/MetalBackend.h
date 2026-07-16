#pragma once

#include "gpu/interface/GpuCapabilities.h"

#include <cstddef>
#include <cstdint>
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

} // namespace ParticleSaturn::Gpu::Metal
