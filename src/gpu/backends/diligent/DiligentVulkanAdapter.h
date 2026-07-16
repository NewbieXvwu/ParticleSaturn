#pragma once

#include "gpu/interface/GpuCapabilities.h"

#include "app/state/AppStates.h"

#include <string>

namespace ParticleSaturn::Gpu::Diligent {

class DiligentVulkanAdapter {
public:
    DiligentVulkanAdapter() = default;
    ~DiligentVulkanAdapter();

    DiligentVulkanAdapter(const DiligentVulkanAdapter&) = delete;
    DiligentVulkanAdapter& operator=(const DiligentVulkanAdapter&) = delete;

    bool Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);
    void Shutdown() noexcept;

    const std::string& AdapterName() const noexcept;
    const GpuCapabilities& Capabilities() const noexcept;

private:
    void* device_ = nullptr;
    void* context_ = nullptr;
    std::string adapterName_;
    GpuCapabilities capabilities_{};
};

} // namespace ParticleSaturn::Gpu::Diligent
