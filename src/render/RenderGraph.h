#pragma once

#include "gpu/interface/GpuTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ParticleSaturn::Render {

struct GraphResource {
    std::string name;
    Gpu::TextureDesc desc;
};

struct ResourceAccess {
    std::uint32_t resource = 0;
    Gpu::ResourceUsage usage = Gpu::ResourceUsage::Undefined;
};

struct RenderPass {
    std::string name;
    std::vector<ResourceAccess> reads;
    std::vector<ResourceAccess> writes;
};

class RenderGraph {
public:
    std::uint32_t AddResource(GraphResource resource);
    std::uint32_t AddPass(std::string name);
    void Read(std::uint32_t pass, std::uint32_t resource, Gpu::ResourceUsage usage);
    void Write(std::uint32_t pass, std::uint32_t resource, Gpu::ResourceUsage usage);
    std::vector<std::uint32_t> Compile() const;

    const std::vector<RenderPass>& Passes() const noexcept;

private:
    void ValidatePass(std::uint32_t pass) const;
    void ValidateResource(std::uint32_t resource) const;

    std::vector<GraphResource> resources_;
    std::vector<RenderPass> passes_;
};

} // namespace ParticleSaturn::Render
