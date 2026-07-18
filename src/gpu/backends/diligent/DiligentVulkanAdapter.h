#pragma once

#include "gpu/interface/GpuDevice.h"

#include "app/state/AppStates.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ParticleSaturn::UI { class ImGuiDiligent; }

namespace ParticleSaturn::Gpu::Diligent {

std::string_view ClassifyDiligentVulkanMessage(std::string_view message) noexcept;

class DiligentVulkanAdapter final : public GpuDevice, private CommandList {
public:
    DiligentVulkanAdapter();
    ~DiligentVulkanAdapter();

    DiligentVulkanAdapter(const DiligentVulkanAdapter&) = delete;
    DiligentVulkanAdapter& operator=(const DiligentVulkanAdapter&) = delete;

    bool Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error);
    bool CreateSwapChain(void* nativeView, std::uint32_t width, std::uint32_t height, std::string& error);
    bool ResizeSwapChain(std::uint32_t width, std::uint32_t height);
    bool PresentClearFrame(const float color[4], std::uint32_t syncInterval);
    bool PresentSceneFrame(std::uint32_t syncInterval);
    bool InitializeImGui(void* nativeView, std::string& error);
    void BeginImGuiFrame();
    bool ImGuiReady() const noexcept;
    bool DeviceLost() const noexcept;
    void SetAcrylicSettings(bool enabled, float strength, bool darkMode) noexcept;
    void SetParticleSettings(std::uint32_t particleCount, bool paused) noexcept;
    bool BaselineCaptureRequested() const noexcept;
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
    bool CreateHdrTargets(std::uint32_t width, std::uint32_t height, std::string& error);
    bool CreateToneMapPipeline(std::string& error);
    bool CreateBloomPipelines(std::string& error);
    bool CreateAcrylicPipeline(std::string& error);
    bool CreateParticleInitializationPipeline(std::string& error);
    bool CreateParticleComputePipeline(std::string& error);
    bool SimulateParticles(CommandList& commands);
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
    void* hdrTexture_ = nullptr;
    void* hdrRenderTarget_ = nullptr;
    void* hdrShaderResource_ = nullptr;
    void* uiSceneTexture_ = nullptr;
    void* uiSceneRenderTarget_ = nullptr;
    void* uiSceneShaderResource_ = nullptr;
    void* bloomTexture_ = nullptr;
    void* bloomRenderTarget_ = nullptr;
    void* bloomShaderResource_ = nullptr;
    void* bloomPingTexture_ = nullptr;
    void* bloomPingRenderTarget_ = nullptr;
    void* bloomPingShaderResource_ = nullptr;
    void* uiWeakTexture_ = nullptr;
    void* uiWeakRenderTarget_ = nullptr;
    void* uiWeakShaderResource_ = nullptr;
    void* uiWeakPingTexture_ = nullptr;
    void* uiWeakPingRenderTarget_ = nullptr;
    void* uiWeakPingShaderResource_ = nullptr;
    void* baselineStagingTexture_ = nullptr;
    std::uint32_t baselineStagingWidth_ = 0;
    std::uint32_t baselineStagingHeight_ = 0;
    bool baselineCaptureRequested_ = false;
    bool baselineCaptured_ = false;
    std::string baselinePath_;
    bool deviceLost_ = false;
    std::uint32_t presentedFrameCount_ = 0;
    void* bloomDownsamplePipeline_ = nullptr;
    void* bloomDownsampleBinding_ = nullptr;
    void* bloomDownsampleTextureVariable_ = nullptr;
    void* bloomBlurPipeline_ = nullptr;
    void* bloomBlurBinding_ = nullptr;
    void* bloomBlurTextureVariable_ = nullptr;
    void* acrylicPipeline_ = nullptr;
    void* acrylicBinding_ = nullptr;
    void* acrylicTextureVariable_ = nullptr;
    void* toneMapPipeline_ = nullptr;
    void* toneMapBinding_ = nullptr;
    void* toneMapTextureVariable_ = nullptr;
    void* toneMapBloomVariable_ = nullptr;
    void* particlePipeline_ = nullptr;
    void* particleBinding_ = nullptr;
    void* particleRenderVariable_ = nullptr;
    void* particleComputePipeline_ = nullptr;
    void* particleComputeBinding_ = nullptr;
    void* particleComputeInputVariable_ = nullptr;
    void* particleComputeOutputVariable_ = nullptr;
    void* particleInitializationPipeline_ = nullptr;
    void* particleInitializationBinding_ = nullptr;
    void* particleInitializationOutputVariable_ = nullptr;
    void* particleInitializationConstantsVariable_ = nullptr;
    BufferHandle sceneIndirectArguments_{};
    BufferHandle toneMapConstants_{};
    BufferHandle bloomConstants_{};
    BufferHandle acrylicConstants_{};
    BufferHandle particleBuffers_[3]{};
    BufferHandle particleComputeConstants_{};
    BufferHandle particleInitializationConstants_{};
    BufferHandle particleIndirectArguments_{};
    std::uint32_t particleRenderIndex_ = 0;
    std::uint32_t particleReadIndex_ = 1;
    std::uint32_t particleWriteIndex_ = 2;
    std::uint32_t particleCount_ = 1'200'000;
    bool particlePaused_ = false;
    bool particleCountDirty_ = false;
    bool uiBlurEnabled_ = true;
    float uiBlurStrength_ = 2.0f;
    bool uiDarkMode_ = true;
    std::string adapterName_;
    GpuCapabilities capabilities_{};
    std::vector<BufferEntry> buffers_;
    std::uint64_t submissionValue_ = 0;
    bool commandsOpen_ = false;
    std::unique_ptr<ParticleSaturn::UI::ImGuiDiligent> imgui_;
};

} // namespace ParticleSaturn::Gpu::Diligent
