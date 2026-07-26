#pragma once

#include "ParticleAbi.h"
#include "app/state/AppStates.h"
#include "gpu/interface/GpuCapabilities.h"
#include "gpu/interface/GpuDevice.h"
#include "render/ResourceRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ParticleSaturn::Gpu::Metal {

// Metal 实现共享图形设备契约：受控代际缓冲句柄、显式资源用途过渡、
// 命令提交令牌和延迟销毁语义与 DiligentVulkanAdapter 一致。Metal 的
// 缓冲默认开启硬件冒险跟踪，Transition 因此只维护契约状态机而无需
// 显式屏障；DrawIndirect/Dispatch 经帧路径注册的活动编码器下发。
class MetalDevice final : public Gpu::GpuDevice, private Gpu::CommandList {
public:
    ~MetalDevice() override;

    bool Initialize();
    const GpuCapabilities& Capabilities() const noexcept override;
    void* NativeDevice() const noexcept;
    void* NativeCommandQueue() const noexcept;

    std::string_view Name() const noexcept override;
    BufferHandle CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) override;
    void UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) override;
    void DestroyBuffer(BufferHandle buffer, FrameToken afterFrame) override;
    Gpu::CommandList& BeginCommands() override;
    FrameToken Submit(Gpu::CommandList& commands) override;

    // 帧路径辅助：命令缓冲访问、原生缓冲解析和活动编码器注册。
    void* NativeCommandBuffer() const noexcept;
    bool CommandsOpen() const noexcept;
    void* ResolveBufferNative(BufferHandle buffer) const;
    void SetActiveComputeEncoder(void* encoder, std::uint32_t threadsPerThreadgroup) noexcept;
    void SetActiveRenderEncoder(void* encoder, std::uint32_t primitiveType) noexcept;
    void ClearActiveEncoders() noexcept;

private:
    struct BufferEntry {
        void* buffer = nullptr;
        std::size_t size = 0;
        std::uint32_t generation = 1;
        ResourceUsage usage = ResourceUsage::Undefined;
        std::uint64_t retireAfter = 0;
        bool pendingRelease = false;
    };

    void* ResolveBuffer(BufferHandle buffer) const;
    void ReleaseRetiredBuffers() noexcept;
    void Transition(BufferHandle buffer, ResourceUsage before, ResourceUsage after) override;
    void Transition(TextureHandle texture, ResourceUsage before, ResourceUsage after) override;
    void DrawIndirect(BufferHandle arguments, std::size_t offset) override;
    void Dispatch(std::uint32_t groupsX, std::uint32_t groupsY, std::uint32_t groupsZ) override;

    void* device_ = nullptr;
    void* commandQueue_ = nullptr;
    GpuCapabilities capabilities_{};
    std::vector<BufferEntry> buffers_;
    void* commandBuffer_ = nullptr;
    std::uint64_t submissionValue_ = 0;
    bool commandsOpen_ = false;
    void* activeComputeEncoder_ = nullptr;
    std::uint32_t activeThreadsPerThreadgroup_ = 0;
    void* activeRenderEncoder_ = nullptr;
    std::uint32_t activePrimitiveType_ = 0;
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

class MetalParticleSystem {
public:
    static constexpr std::uint32_t ParticleCount = 1200000;

    using ParticleSnapshot = ShaderAbi::Particle;

    bool Initialize(MetalDevice& device, const char* libraryPath, std::uint32_t seed);
    bool Simulate(float deltaTime, float handScale, bool handTracked, std::uint32_t particleCount);
    // 共享契约版本：要求设备命令已打开，写缓冲经显式用途过渡后由
    // CommandList::Dispatch 调度，随后回到着色器读取并轮转三缓冲。
    bool EncodeSimulation(Gpu::CommandList& commands, float deltaTime, float handScale, bool handTracked,
                          std::uint32_t particleCount);
    bool ReadBack(std::vector<ParticleSnapshot>& particles, std::uint32_t count) const;
    void* RenderBuffer() const noexcept;

private:
    MetalDevice* device_ = nullptr;
    void* commandQueue_ = nullptr;
    void* initializePipeline_ = nullptr;
    void* simulationPipeline_ = nullptr;
    Gpu::BufferHandle bufferHandles_[3]{};
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
    Gpu::BufferHandle handle_{};
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

// 四个后处理类在首次 Encode 时构建计算管线并跨帧持有（AUDIT P1-8），
// libraryPath 变更时重建；对象须跨帧存活缓存才有意义（见 MetalFrameRenderer 成员）。
// tonemap 走单源翻译产物（D-004）：全屏三角 + 生成的 fragment（main0），
// 渲染管线按输出像素格式惰性缓存（drawable=BGRA8 / ui 场景=RGBA16F 两种）。
class MetalToneMapper {
public:
    ~MetalToneMapper();
    bool Apply(MetalDevice& device, const char* libraryPath, void* hdrTexture, void* bloomTexture, void* outputTexture,
               std::uint32_t width, std::uint32_t height, float bloomStrength, bool transparent = false);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* hdrTexture,
                void* bloomTexture, void* outputTexture, std::uint32_t width, std::uint32_t height,
                float bloomStrength, bool transparent = false);

private:
    bool EnsureFunctions(MetalDevice& device, const char* libraryPath);
    void* PipelineFor(MetalDevice& device, std::uint32_t pixelFormat);

    void* vertexFunction_ = nullptr;
    void* fragmentFunction_ = nullptr;
    std::vector<std::pair<std::uint32_t, void*>> pipelines_;
    std::string libraryPath_;
};

// bloom 走单源翻译产物（D-004）：降采样/模糊为全屏 fragment（入口
// BloomDownsampleFragment/KawaseBlurFragment），PSO 按输出格式惰性缓存。
class MetalBloom {
public:
    ~MetalBloom();
    bool Apply(MetalDevice& device, const char* libraryPath, void* sceneHdr, void* bloomA, void* bloomB,
               std::uint32_t width, std::uint32_t height, float blurStrength);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* sceneHdr,
                void* bloomA, void* bloomB, std::uint32_t width, std::uint32_t height, float blurStrength);

private:
    bool EnsurePipelines(MetalDevice& device, const char* libraryPath);
    void* vertexFunction_ = nullptr;
    void* downsampleFragment_ = nullptr;
    void* blurFragment_ = nullptr;
    std::vector<std::pair<std::uint32_t, void*>> downsamplePipelines_;
    std::vector<std::pair<std::uint32_t, void*>> blurPipelines_;
    std::string libraryPath_;
};

class MetalAcrylic {
public:
    ~MetalAcrylic();
    bool Apply(MetalDevice& device, const char* libraryPath, void* uiSceneTexture, void* blurA, void* blurB,
               void* blurWeakA, void* blurWeakB, void* outputTexture, void* weakOutputTexture,
               std::uint32_t width, std::uint32_t height, float blurStrength);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* uiSceneTexture,
                void* blurA, void* blurB, void* blurWeakA, void* blurWeakB, void* outputTexture,
                void* weakOutputTexture, std::uint32_t width, std::uint32_t height, float blurStrength);

private:
    bool EnsurePipelines(MetalDevice& device, const char* libraryPath);
    void* vertexFunction_ = nullptr;
    void* downsampleFragment_ = nullptr;
    void* blurFragment_ = nullptr;
    void* composite_ = nullptr;  // 合成核暂保留手写 compute
    std::vector<std::pair<std::uint32_t, void*>> downsamplePipelines_;
    std::vector<std::pair<std::uint32_t, void*>> blurPipelines_;
    std::string libraryPath_;
};

class MetalSevenSegmentFps {
public:
    ~MetalSevenSegmentFps();
    bool Render(MetalDevice& device, const char* libraryPath, void* outputTexture, std::uint32_t width,
                std::uint32_t height, std::uint32_t framesPerSecond);
    bool Encode(MetalDevice& device, void* nativeCommandBuffer, const char* libraryPath, void* outputTexture,
                std::uint32_t width, std::uint32_t height, std::uint32_t framesPerSecond);

private:
    bool EnsurePipelines(MetalDevice& device, const char* libraryPath);
    void* pipeline_ = nullptr;
    std::string libraryPath_;
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
    // Metal 3 object/mesh shader 管线是否可用（能力申报用）。
    bool ObjectShaderAvailable() const noexcept { return objectShaderPipeline_ != nullptr; }
    void Draw(void* encoder, void* particleBuffer, void* starBuffer, std::uint32_t width, std::uint32_t height,
              const App::AppState& state);
    // 共享契约路径：受控间接参数缓冲在渲染编码器创建前更新并过渡到
    // 间接参数状态，粒子经 CommandList::DrawIndirect 下发。
    bool PrepareIndirectArguments(MetalDevice& device, Gpu::CommandList& commands, std::uint32_t particleCount);
    void Draw(MetalDevice& device, Gpu::CommandList& commands, void* encoder, void* particleBuffer, void* starBuffer,
              std::uint32_t width, std::uint32_t height, const App::AppState& state);

private:
    void DrawInternal(MetalDevice* device, Gpu::CommandList* commands, void* encoder, void* particleBuffer,
                      void* starBuffer, std::uint32_t width, std::uint32_t height, const App::AppState& state);

    void* particlePipeline_ = nullptr;
    void* starPipeline_ = nullptr;
    void* objectShaderPipeline_ = nullptr;
    MetalIndirectDraw particleIndirect_;
    Gpu::BufferHandle managedIndirect_{};
    std::uint32_t managedIndirectCount_ = 0;
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
    // 跨帧持有，管线只在首帧构建（AUDIT P1-8）。
    MetalBloom bloom_;
    MetalToneMapper toneMapper_;
    MetalAcrylic acrylic_;
    MetalSevenSegmentFps fps_;
};

} // namespace ParticleSaturn::Gpu::Metal
