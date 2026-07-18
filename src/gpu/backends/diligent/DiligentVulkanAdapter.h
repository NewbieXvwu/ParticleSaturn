#pragma once

#include "gpu/interface/GpuDevice.h"

#include "app/state/AppStates.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ParticleSaturn::Gpu::Diligent {

class DiligentVulkanAdapter final : public GpuDevice, private CommandList {
public:
    DiligentVulkanAdapter() = default;
    ~DiligentVulkanAdapter();

    DiligentVulkanAdapter(const DiligentVulkanAdapter&) = delete;
    DiligentVulkanAdapter& operator=(const DiligentVulkanAdapter&) = delete;

    bool Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);
    bool CreateSwapChain(void* nativeView, std::uint32_t width, std::uint32_t height, std::string& error);
    bool ResizeSwapChain(std::uint32_t width, std::uint32_t height);
    bool PresentClearFrame(const float color[4], std::uint32_t syncInterval);
    bool PresentSceneFrame(std::uint32_t syncInterval);
    void Shutdown() noexcept;

    std::string_view Name() const noexcept override;
    const std::string& AdapterName() const noexcept;
    const GpuCapabilities& Capabilities() const noexcept override;
    BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) override;
    void UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) override;
    void DestroyBuffer(BufferHandle buffer, FrameToken afterFrame) override;
    CommandList& BeginCommands() override;
    FrameToken Submit(CommandList& commands) override;

private:
    struct BufferEntry {
        void* buffer = nullptr;
        std::size_t size = 0;
        std::uint32_t generation = 1;
        ResourceUsage usage = ResourceUsage::Undefined;
        std::uint64_t retireAfter = 0;
        bool pendingRelease = false;
    };

    bool CreateScenePipeline(std::string& error);
    bool CreateParticlePipeline(std::string& error);
    void ReleaseRetiredBuffers() noexcept;
    void* ResolveBuffer(BufferHandle buffer) const;
    void Transition(BufferHandle buffer, ResourceUsage before, ResourceUsage after) override;
    void Transition(TextureHandle texture, ResourceUsage before, ResourceUsage after) override;
    void DrawIndirect(BufferHandle arguments, std::size_t offset) override;
    void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;

    void* device_ = nullptr;
    void* context_ = nullptr;
    void* swapChain_ = nullptr;
    void* scenePipeline_ = nullptr;
    void* particlePipeline_ = nullptr;
    void* particleBinding_ = nullptr;
    BufferHandle sceneIndirectArguments_{};
    BufferHandle particleBuffer_{};
    std::string adapterName_;
    GpuCapabilities capabilities_{};
    std::vector<BufferEntry> buffers_;
    std::uint64_t submissionValue_ = 0;
    bool commandsOpen_ = false;
};

} // namespace ParticleSaturn::Gpu::Diligent
