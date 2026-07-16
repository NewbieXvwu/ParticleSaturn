#include "RenderGraph.h"

#include <algorithm>
#include <stdexcept>

namespace ParticleSaturn::Render {

std::uint32_t RenderGraph::AddResource(GraphResource resource) {
    if (resource.name.empty() || resource.desc.width == 0 || resource.desc.height == 0) {
        throw std::invalid_argument{"render graph resources require a name and non-zero dimensions"};
    }
    resources_.push_back(std::move(resource));
    return static_cast<std::uint32_t>(resources_.size() - 1);
}

std::uint32_t RenderGraph::AddPass(std::string name) {
    if (name.empty()) {
        throw std::invalid_argument{"render graph passes require a name"};
    }
    passes_.push_back({std::move(name), {}, {}});
    return static_cast<std::uint32_t>(passes_.size() - 1);
}

void RenderGraph::Read(std::uint32_t pass, std::uint32_t resource, Gpu::ResourceUsage usage) {
    ValidatePass(pass);
    ValidateResource(resource);
    passes_[pass].reads.push_back({resource, usage});
}

void RenderGraph::Write(std::uint32_t pass, std::uint32_t resource, Gpu::ResourceUsage usage) {
    ValidatePass(pass);
    ValidateResource(resource);
    passes_[pass].writes.push_back({resource, usage});
}

std::vector<std::uint32_t> RenderGraph::Compile() const {
    std::vector<std::vector<std::uint32_t>> dependencies(passes_.size());
    std::vector<std::uint32_t> lastWriter(resources_.size(), static_cast<std::uint32_t>(-1));

    for (std::uint32_t pass = 0; pass < passes_.size(); ++pass) {
        const auto addDependency = [&](std::uint32_t resource) {
            const auto writer = lastWriter[resource];
            if (writer != static_cast<std::uint32_t>(-1) && writer != pass) {
                dependencies[pass].push_back(writer);
            }
        };
        for (const auto& read : passes_[pass].reads) {
            addDependency(read.resource);
        }
        for (const auto& write : passes_[pass].writes) {
            addDependency(write.resource);
            lastWriter[write.resource] = pass;
        }
        std::sort(dependencies[pass].begin(), dependencies[pass].end());
        dependencies[pass].erase(std::unique(dependencies[pass].begin(), dependencies[pass].end()), dependencies[pass].end());
    }

    std::vector<std::uint32_t> order;
    std::vector<bool> emitted(passes_.size(), false);
    while (order.size() != passes_.size()) {
        bool progressed = false;
        for (std::uint32_t pass = 0; pass < passes_.size(); ++pass) {
            if (emitted[pass]) {
                continue;
            }
            const bool ready = std::all_of(dependencies[pass].begin(), dependencies[pass].end(),
                [&emitted](std::uint32_t dependency) { return emitted[dependency]; });
            if (ready) {
                emitted[pass] = true;
                order.push_back(pass);
                progressed = true;
            }
        }
        if (!progressed) {
            throw std::logic_error{"render graph contains a dependency cycle"};
        }
    }
    return order;
}

const std::vector<RenderPass>& RenderGraph::Passes() const noexcept {
    return passes_;
}

void RenderGraph::ValidatePass(std::uint32_t pass) const {
    if (pass >= passes_.size()) {
        throw std::out_of_range{"render graph pass does not exist"};
    }
}

void RenderGraph::ValidateResource(std::uint32_t resource) const {
    if (resource >= resources_.size()) {
        throw std::out_of_range{"render graph resource does not exist"};
    }
}

} // namespace ParticleSaturn::Render
