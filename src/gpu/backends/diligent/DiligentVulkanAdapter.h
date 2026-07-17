#pragma once

#include "gpu/interface/GpuCapabilities.h"

#include "app/state/AppStates.h"

#include <cstdint>
#include <string>

namespace ParticleSaturn::Gpu::Diligent {

class DiligentVulkanAdapter {
public:
    DiligentVulkanAdapter() = default;
    ~DiligentVulkanAdapter();

    DiligentVulkanAdapter(const DiligentVulkanAdapter&) = delete;
    DiligentVulkanAdapter& operator=(const DiligentVulkanAdapter&) = delete;

    bool Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);
    bool CreateSwapChain(void* nativeView, std::uint32_t width, std::uint32_t height, std::string& error);
    bool ResizeSwapChain(std::uint32_t width, std::uint32_t height);
    bool PresentClearFrame(const float color[4], std::uint32_t syncInterval);
    void Shutdown() noexcept;

    const std::string& AdapterName() const noexcept;
    const GpuCapabilities& Capabilities() const noexcept;

private:
    void* device_ = nullptr;
    void* context_ = nullptr;
    void* swapChain_ = nullptr;
    std::string adapterName_;
    GpuCapabilities capabilities_{};
};

} // namespace ParticleSaturn::Gpu::Diligent
