#pragma once

#include "gpu/interface/GpuCapabilities.h"

#include <cstddef>
#include <cstdint>

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

} // namespace ParticleSaturn::Gpu::Metal
