#include "DiligentVulkanAdapter.h"

#include "render/RenderGraph.h"
#include "Diligent/DiligentShaderSources.h"
#include "Diligent/ImGuiDiligent.h"
#include "services/diagnostics/DiagnosticBus.h"
#include "services/vulkan/VulkanDriverRuntime.h"

#include <EngineFactoryVk.h>
#include <Buffer.h>
#include <RenderDevice.h>
#include <DeviceContext.h>
#include <PipelineState.h>
#include <Shader.h>
#include <SwapChain.h>
#include <MacOSNativeWindow.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ParticleSaturn::Gpu::Diligent {

namespace {
std::atomic_bool gDeviceLostReported{false};
std::atomic_bool gDeviceLostInjected{false};
}

std::string_view ClassifyDiligentVulkanMessage(std::string_view message) noexcept {
    const bool deviceLost = message.find("DEVICE_LOST") != std::string_view::npos ||
                            message.find("device lost") != std::string_view::npos;
    const bool swapChainIssue = message.find("OUT_OF_DATE") != std::string_view::npos ||
                                message.find("SUBOPTIMAL") != std::string_view::npos ||
                                message.find("Present") != std::string_view::npos;
    return deviceLost ? "device-lost" : swapChainIssue ? "swap-chain" : "diligent-error";
}

namespace {

bool IsEnabled(::Diligent::DEVICE_FEATURE_STATE state) {
    return state != ::Diligent::DEVICE_FEATURE_STATE_DISABLED;
}

bool WriteMappedBaseline(const ::Diligent::MappedTextureSubresource& mapped,
                         std::uint32_t width, std::uint32_t height, const char* path) {
    if (mapped.pData == nullptr || mapped.Stride < static_cast<std::uint64_t>(width) * 4U ||
        path == nullptr || path[0] == '\0' || width == 0 || height == 0) return false;
    std::ofstream output{path, std::ios::binary};
    if (!output) return false;
    output << "P6\n" << width << ' ' << height << "\n255\n";
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t row = 0; row < height; ++row) {
        const auto* source = bytes + static_cast<std::size_t>(row) * mapped.Stride;
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto* pixel = source + static_cast<std::size_t>(column) * 4U;
            output.write(reinterpret_cast<const char*>(pixel), 3);
        }
    }
    return output.good();
}

void DILIGENT_CALL_TYPE ReportDiligentVulkanMessage(::Diligent::DEBUG_MESSAGE_SEVERITY severity,
                                                     const ::Diligent::Char* message,
                                                     const ::Diligent::Char*, const ::Diligent::Char*, int) {
    if (message == nullptr || severity < ::Diligent::DEBUG_MESSAGE_SEVERITY_ERROR) return;
    const std::string_view text{message};
    const auto code = ClassifyDiligentVulkanMessage(text);
    if (code == "device-lost") gDeviceLostReported.store(true, std::memory_order_release);
    Services::Diagnostics::DiagnosticBus::Instance().Publish(
        "vulkan", std::string{code}, std::string{text}, Services::Diagnostics::Severity::Error);
}

constexpr const char* SceneVertexShader = R"(
layout(location = 0) out vec2 uv;
const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main() {
    vec2 position = positions[gl_VertexIndex];
    uv = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

constexpr const char* SceneFragmentShader = R"(
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

float hash(vec2 value) {
    return fract(sin(dot(value, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    const vec3 space = vec3(0.002, 0.003, 0.008);
    vec3 result = space;
    vec2 cell = floor(uv * vec2(170.0, 100.0));
    float star = step(0.997, hash(cell));
    result += vec3(star) * (0.18 + 0.7 * hash(cell + 13.0));
    color = vec4(result, 1.0);
}
)";

constexpr const char* ParticleVertexShader = R"(
struct Particle { vec4 position; uint color; float speed; uint isRing; uint padding; };
layout(set=0, binding=0, std430) readonly buffer gParticles { Particle particles[]; } particleBuffer;
layout(set=0, binding=1, std140) uniform RenderConstants {
    vec4 uScene;
    vec4 uViewport;
};
layout(location = 0) out vec4 particleColor;
layout(location = 1) out vec2 particleUv;
layout(location = 2) out float particleDistance;
layout(location = 3) out float particleScale;
layout(location = 4) out float particleIsRing;
layout(location = 5) out float particleDensity;

vec3 rotateSaturn(vec3 position) {
    const float cz = cos(0.466);
    const float sz = sin(0.466);
    const float cy = cos(uScene.w);
    const float sy = sin(uScene.w);
    const float cx = cos(uScene.z);
    const float sx = sin(uScene.z);
    const vec3 zRotated = vec3(position.x * cz - position.y * sz,
                               position.x * sz + position.y * cz, position.z);
    const vec3 yRotated = vec3(zRotated.x * cy + zRotated.z * sy, zRotated.y,
                              -zRotated.x * sy + zRotated.z * cy);
    return vec3(yRotated.x, yRotated.y * cx - yRotated.z * sx,
                yRotated.y * sx + yRotated.z * cx);
}

void main() {
    const vec2 corners[6] = vec2[6](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    Particle particle = particleBuffer.particles[gl_InstanceIndex];
    const vec2 corner = corners[gl_VertexIndex];
    const vec3 position = rotateSaturn(particle.position.xyz * uScene.y);
    const float distance = 100.0 - position.z;
    const float projectedDistance = max(distance, 0.001);
    const float focalLength = 1.0 / tan(1.047 * 0.5);
    const float aspect = max(uViewport.x, 0.001);
    const vec2 center = vec2(position.x * focalLength / (aspect * projectedDistance),
                             position.y * focalLength / projectedDistance);
    const float nearMask = distance <= 50.0 ? 1.0 : 0.0;
    const float ringFactor = mix(mix(1.0, 0.8, nearMask), 1.0, float(particle.isRing));
    const float pointSize = particle.position.w * 350.0 * 0.55 / max(distance, 0.1) *
                            (uViewport.y / 1080.0) * ringFactor * pow(max(uViewport.z, 0.0001), 0.8);
    const float pixelSize = clamp(pointSize, 0.0, 300.0 * (uViewport.y / 1080.0));
    const vec2 extent = corner * (pixelSize * 0.5) * vec2(2.0 / (aspect * uViewport.y), 2.0 / uViewport.y);
    gl_Position = vec4(center + extent, 0.0, 1.0);
    particleColor = vec4(float(particle.color & 255u), float((particle.color >> 8u) & 255u),
                         float((particle.color >> 16u) & 255u), float((particle.color >> 24u) & 255u)) / 255.0;
    particleUv = corner * 0.5 + 0.5;
    particleDistance = distance;
    particleScale = uScene.y;
    particleIsRing = float(particle.isRing);
    particleDensity = uViewport.w;
}
)";

constexpr const char* ParticleFragmentShader = R"(
layout(location = 0) in vec4 particleColor;
layout(location = 1) in vec2 particleUv;
layout(location = 2) in float particleDistance;
layout(location = 3) in float particleScale;
layout(location = 4) in float particleIsRing;
layout(location = 5) in float particleDensity;
layout(location = 0) out vec4 color;
void main() {
    const vec2 centered = particleUv * 2.0 - 1.0;
    const float radiusSquared = dot(centered, centered);
    if (radiusSquared > 1.0) discard;
    const float glow = smoothstep(1.0, 0.4, radiusSquared);
    const float t = clamp((particleScale - 0.15) * 0.4255, 0.0, 1.0);
    const float smoothedT = smoothstep(0.1, 0.9, t);
    vec3 finalColor = mix(vec3(0.35, 0.22, 0.05), particleColor.rgb, smoothedT) * (0.2 + t);
    const float closeMix = smoothstep(40.0, 0.0, particleDistance);
    const vec3 ringColor = finalColor + vec3(0.15, 0.12, 0.1) * closeMix;
    const vec3 bodyColor = mix(finalColor, pow(particleColor.rgb, vec3(1.4)) * 1.5, closeMix * 0.8);
    finalColor = mix(bodyColor, ringColor, particleIsRing);
    const float depthAlpha = smoothstep(0.0, 10.0, particleDistance);
    const float alpha = glow * particleColor.a * (0.25 + 0.45 * smoothstep(0.0, 0.5, t)) *
                        depthAlpha * particleDensity;
    color = vec4(finalColor, alpha);
}
)";

constexpr const char* ParticleComputeShader = R"(
struct Particle { vec4 position; uint color; float speed; uint isRing; uint padding; };
layout(set=0, binding=0, std430) readonly buffer gParticlesIn { Particle particlesIn[]; } particleInput;
layout(set=0, binding=1, std430) writeonly buffer gParticlesOut { Particle particlesOut[]; } particleOutput;
layout(set=0, binding=2, std140) uniform ComputeConstants {
    float uDt;
    float uHandScale;
    float uHandHas;
    uint uParticleCount;
};
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
void main() {
    const uint id = gl_GlobalInvocationID.x;
    if (id >= uParticleCount) return;
    const Particle particle = particleInput.particlesIn[id];
    const float timeFactor = mix(1.0, uHandScale, uHandHas);
    const float angle = particle.isRing == 0u ? 0.03 * uDt * timeFactor
                                               : particle.speed * 0.2 * uDt * timeFactor;
    const float c = cos(angle);
    const float s = sin(angle);
    Particle updated = particle;
    updated.position.x = particle.position.x * c - particle.position.z * s;
    updated.position.z = particle.position.x * s + particle.position.z * c;
    particleOutput.particlesOut[id] = updated;
}
)";

struct Particle {
    float position[4];
    std::uint32_t color;
    float speed;
    std::uint32_t isRing;
    std::uint32_t padding;
};

struct ParticleComputeConstants {
    float deltaTime;
    float handScale;
    float handHas;
    std::uint32_t particleCount;
};

struct ParticleRenderConstants {
    float scene[4];
    float viewport[4];
};

struct ParticleInitializationConstants {
    std::uint32_t particleCount;
    std::uint32_t seed;
    float radius;
    float padding;
};

struct ToneMapConstants {
    float bloomStrength;
    float transparent;
    float padding[2];
};

struct AcrylicConstants {
    float tint[4];
    float params[4];
};

static_assert(sizeof(Particle) == 32);
static_assert(sizeof(ParticleComputeConstants) == 16);
static_assert(sizeof(ParticleRenderConstants) == 32);
static_assert(sizeof(ParticleInitializationConstants) == 16);
static_assert(sizeof(ToneMapConstants) == 16);
static_assert(sizeof(AcrylicConstants) == 32);

constexpr std::uint32_t MaxParticleCount = 1'200'000;

::Diligent::RESOURCE_STATE ToDiligentResourceState(ResourceUsage usage) {
    switch (usage) {
        case ResourceUsage::Undefined: return ::Diligent::RESOURCE_STATE_UNKNOWN;
        case ResourceUsage::CopySource: return ::Diligent::RESOURCE_STATE_COPY_SOURCE;
        case ResourceUsage::CopyDestination: return ::Diligent::RESOURCE_STATE_COPY_DEST;
        case ResourceUsage::ShaderRead: return ::Diligent::RESOURCE_STATE_SHADER_RESOURCE;
        case ResourceUsage::ShaderWrite: return ::Diligent::RESOURCE_STATE_UNORDERED_ACCESS;
        case ResourceUsage::IndirectArgument: return ::Diligent::RESOURCE_STATE_INDIRECT_ARGUMENT;
        case ResourceUsage::RenderTarget:
        case ResourceUsage::Present:
            throw std::invalid_argument{"a buffer cannot use a texture-only resource state"};
    }
    throw std::invalid_argument{"unknown resource usage"};
}

::Diligent::BIND_FLAGS ToDiligentBindFlags(BufferUsage usage) {
    auto flags = ::Diligent::BIND_NONE;
    if (HasUsage(usage, BufferUsage::Vertex)) flags |= ::Diligent::BIND_VERTEX_BUFFER;
    if (HasUsage(usage, BufferUsage::Index)) flags |= ::Diligent::BIND_INDEX_BUFFER;
    if (HasUsage(usage, BufferUsage::Uniform)) flags |= ::Diligent::BIND_UNIFORM_BUFFER;
    if (HasUsage(usage, BufferUsage::Storage)) flags |= ::Diligent::BIND_SHADER_RESOURCE | ::Diligent::BIND_UNORDERED_ACCESS;
    if (HasUsage(usage, BufferUsage::Indirect)) flags |= ::Diligent::BIND_INDIRECT_DRAW_ARGS;
    return flags;
}

} // namespace

DiligentVulkanAdapter::DiligentVulkanAdapter() = default;

DiligentVulkanAdapter::~DiligentVulkanAdapter() {
    Shutdown();
}

bool DiligentVulkanAdapter::Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error) {
    Shutdown();
    const char* baselinePath = std::getenv("PARTICLESATURN_CAPTURE_BASELINE");
    baselineCaptureRequested_ = baselinePath != nullptr && baselinePath[0] != '\0';
    baselineCaptured_ = false;
    baselinePath_ = baselineCaptureRequested_ ? baselinePath : std::string{};
    if (!Services::Vulkan::ConfigureDriver(driver, bundleResources, error)) return false;

    ::Diligent::IEngineFactoryVk* factory = ::Diligent::GetEngineFactoryVk();
    if (factory == nullptr) {
        error = "Diligent Vulkan factory is unavailable";
        return false;
    }
    factory->SetMessageCallback(&ReportDiligentVulkanMessage);
    ::Diligent::EngineVkCreateInfo createInfo;
    createInfo.EnableValidation = false;
    ::Diligent::IRenderDevice* device = nullptr;
    ::Diligent::IDeviceContext* context = nullptr;
    factory->CreateDeviceAndContextsVk(createInfo, &device, &context);
    if (device == nullptr || context == nullptr) {
        if (context != nullptr) context->Release();
        if (device != nullptr) device->Release();
        error = "Diligent Vulkan could not create a device for the selected ICD";
        return false;
    }

    const auto& adapter = device->GetAdapterInfo();
    adapterName_ = adapter.Description;
    capabilities_.supportsCompute = IsEnabled(adapter.Features.ComputeShaders);
    capabilities_.supportsStorageBuffer = true;
    capabilities_.supportsIndirectDraw =
        (adapter.DrawCommand.CapFlags & ::Diligent::DRAW_COMMAND_CAP_FLAG_DRAW_INDIRECT) != 0;
    capabilities_.supportsMeshShader = IsEnabled(adapter.Features.MeshShaders);
    capabilities_.supportsTransparentSurface = true;
    capabilities_.supportsTimestamp = IsEnabled(adapter.Features.TimestampQueries);
    capabilities_.supportsProgramCache = true;
    capabilities_.supportsAdaptiveVSync = false;
    device_ = device;
    context_ = context;
    error.clear();
    return true;
}

bool DiligentVulkanAdapter::CreateSwapChain(void* nativeView, std::uint32_t width, std::uint32_t height,
                                             std::string& error) {
    if (device_ == nullptr || context_ == nullptr) {
        error = "Diligent Vulkan device is not initialized";
        return false;
    }
    if (nativeView == nullptr || width == 0 || height == 0) {
        error = "Vulkan swap chain requires a non-empty macOS view and drawable size";
        return false;
    }
    if (swapChain_ != nullptr) static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
    swapChain_ = nullptr;
    ::Diligent::SwapChainDesc description{width, height, ::Diligent::TEX_FORMAT_RGBA8_UNORM_SRGB,
                                           ::Diligent::TEX_FORMAT_D32_FLOAT};
    ::Diligent::ISwapChain* swapChain = nullptr;
    ::Diligent::GetEngineFactoryVk()->CreateSwapChainVk(
        static_cast<::Diligent::IRenderDevice*>(device_), static_cast<::Diligent::IDeviceContext*>(context_),
        description, ::Diligent::NativeWindow{nativeView}, &swapChain);
    if (swapChain == nullptr) {
        error = "Diligent Vulkan could not create a macOS swap chain";
        return false;
    }
    swapChain_ = swapChain;
    if (!CreateHdrTargets(width, height, error)) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
        return false;
    }
    if (!CreateScenePipeline(error)) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
        return false;
    }
    if (!CreateParticlePipeline(error)) return false;
    if (!CreateToneMapPipeline(error)) return false;
    if (!CreateBloomPipelines(error)) return false;
    if (!CreateAcrylicPipeline(error)) return false;
    if (!sceneIndirectArguments_) {
        const std::array<std::uint32_t, 4> arguments{3, 1, 0, 0};
        try {
            sceneIndirectArguments_ = CreateBuffer(
                {sizeof(arguments), 0, BufferUsage::Indirect}, std::as_bytes(std::span{arguments}));
            auto& commands = BeginCommands();
            commands.Transition(sceneIndirectArguments_, ResourceUsage::Undefined, ResourceUsage::IndirectArgument);
            static_cast<void>(Submit(commands));
        } catch (const std::exception& exception) {
            error = exception.what();
            static_cast<::Diligent::IPipelineState*>(scenePipeline_)->Release();
            scenePipeline_ = nullptr;
            static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
            swapChain_ = nullptr;
            return false;
        }
    }
    error.clear();
    return true;
}

bool DiligentVulkanAdapter::ResizeSwapChain(std::uint32_t width, std::uint32_t height) {
    if (swapChain_ == nullptr || width == 0 || height == 0) return false;
    static_cast<::Diligent::IDeviceContext*>(context_)->Flush();
    static_cast<::Diligent::ISwapChain*>(swapChain_)->Resize(width, height);
    std::string error;
    return CreateHdrTargets(width, height, error);
}

bool DiligentVulkanAdapter::PresentClearFrame(const float color[4], std::uint32_t syncInterval) {
    if (swapChain_ == nullptr || color == nullptr) return false;
    auto* swapChain = static_cast<::Diligent::ISwapChain*>(swapChain_);
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    auto* target = swapChain->GetCurrentBackBufferRTV();
    if (target == nullptr) return false;
    context->ClearRenderTarget(target, color, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    swapChain->Present(syncInterval);
    return true;
}

bool DiligentVulkanAdapter::PresentSceneFrame(std::uint32_t syncInterval) {
    if (swapChain_ == nullptr || scenePipeline_ == nullptr || particlePipeline_ == nullptr ||
        particleComputePipeline_ == nullptr || context_ == nullptr) return false;
    ++presentedFrameCount_;
    const char* injection = std::getenv("PARTICLESATURN_VULKAN_DEVICE_LOST_SMOKE");
    if (presentedFrameCount_ == 2 && injection != nullptr && std::string_view{injection} == "1" &&
        !gDeviceLostInjected.exchange(true, std::memory_order_acq_rel)) {
        ReportDiligentVulkanMessage(::Diligent::DEBUG_MESSAGE_SEVERITY_ERROR,
                                    "test injected VK_ERROR_DEVICE_LOST", nullptr, nullptr, 0);
    }
    if (gDeviceLostReported.exchange(false, std::memory_order_acq_rel)) {
        deviceLost_ = true;
        return false;
    }
    auto* swapChain = static_cast<::Diligent::ISwapChain*>(swapChain_);
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    auto* target = swapChain->GetCurrentBackBufferRTV();
    if (target == nullptr || !sceneIndirectArguments_ || !particleIndirectArguments_) return false;
    const auto& description = swapChain->GetDesc();
    auto& commands = BeginCommands();
    const ParticleRenderConstants renderConstants{
        {sceneTime_, sceneScale_, sceneRotationX_, sceneRotationY_},
        {description.Height == 0 ? 1.0f : static_cast<float>(description.Width) / static_cast<float>(description.Height),
         static_cast<float>(description.Height), pixelRatio_, densityCompensation_}};
    UpdateBuffer(particleRenderConstants_, 0, std::as_bytes(std::span{&renderConstants, 1}));
    Render::RenderGraph graph;
    const auto drawable = graph.AddResource({"vulkan-drawable", {description.Width, description.Height, 1}});
    const auto hdr = graph.AddResource({"vulkan-hdr-scene", {description.Width, description.Height, 1}});
    const auto bloom = graph.AddResource({"vulkan-bloom", {std::max(1u, description.Width / 6u), std::max(1u, description.Height / 6u), 1}});
    const auto uiScene = graph.AddResource({"vulkan-ui-scene", {description.Width, description.Height, 1}});
    const auto uiWeak = graph.AddResource({"vulkan-ui-blur-weak", {std::max(1u, description.Width / 12u),
                                                                      std::max(1u, description.Height / 12u), 1}});
    const auto particles = graph.AddResource({"vulkan-particles", {1, 1, 1}});
    const auto simulation = graph.AddPass("vulkan-particle-simulation", [&] {
        return SimulateParticles(commands);
    });
    const auto scene = graph.AddPass("vulkan-scene", [&] {
        auto* hdrTarget = static_cast<::Diligent::ITextureView*>(hdrRenderTarget_);
        context->SetRenderTargets(1, &hdrTarget, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(scenePipeline_));
        commands.DrawIndirect(sceneIndirectArguments_, 0);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(particlePipeline_));
        auto* particleView = static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleBuffers_[particleRenderIndex_]))->GetDefaultView(::Diligent::BUFFER_VIEW_SHADER_RESOURCE);
        static_cast<::Diligent::IShaderResourceVariable*>(particleRenderVariable_)->Set(particleView);
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(particleBinding_), ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        commands.DrawIndirect(particleIndirectArguments_, 0);
        return true;
    });
    void* bloomOutputView = bloomShaderResource_;
    const auto downsample = graph.AddPass("vulkan-bloom-downsample", [&] {
        const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
            1.0f / static_cast<float>(std::max(1u, description.Width / 6u)),
            1.0f / static_cast<float>(std::max(1u, description.Height / 6u))}, 0.0f, 1.0f};
        UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
        auto* renderTargetView = static_cast<::Diligent::ITextureView*>(bloomRenderTarget_);
        context->SetRenderTargets(1, &renderTargetView, nullptr,
                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomDownsamplePipeline_));
        static_cast<::Diligent::IShaderResourceVariable*>(bloomDownsampleTextureVariable_)->Set(
            static_cast<::Diligent::ITextureView*>(hdrShaderResource_));
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomDownsampleBinding_),
                                       ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ::Diligent::DrawAttribs draw;
        draw.NumVertices = 4;
        draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw(draw);
        bloomOutputView = bloomShaderResource_;
        return true;
    });
    constexpr std::array<float, 7> bloomOffsets{0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    std::vector<std::uint32_t> blurPasses;
    blurPasses.reserve(bloomOffsets.size());
    for (std::uint32_t index = 0; index < bloomOffsets.size(); ++index) {
        blurPasses.push_back(graph.AddPass("vulkan-bloom-blur-" + std::to_string(index), [&, index] {
            const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
                1.0f / static_cast<float>(std::max(1u, description.Width / 6u)),
                1.0f / static_cast<float>(std::max(1u, description.Height / 6u))}, bloomOffsets[index], 0.0f};
            UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
            void* renderTarget = index % 2 == 0 ? bloomPingRenderTarget_ : bloomRenderTarget_;
            void* shaderResource = index % 2 == 0 ? bloomPingShaderResource_ : bloomShaderResource_;
            auto* renderTargetView = static_cast<::Diligent::ITextureView*>(renderTarget);
            context->SetRenderTargets(1, &renderTargetView, nullptr,
                                      ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomBlurPipeline_));
            static_cast<::Diligent::IShaderResourceVariable*>(bloomBlurTextureVariable_)->Set(
                static_cast<::Diligent::ITextureView*>(bloomOutputView));
            context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomBlurBinding_),
                                           ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            ::Diligent::DrawAttribs draw;
            draw.NumVertices = 4;
            draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
            context->Draw(draw);
            bloomOutputView = shaderResource;
            return true;
        }));
    }
    const auto toneMap = graph.AddPass("vulkan-tone-map", [&] {
        auto* uiSceneTarget = static_cast<::Diligent::ITextureView*>(uiSceneRenderTarget_);
        context->SetRenderTargets(1, &uiSceneTarget, nullptr,
                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(toneMapPipeline_));
        auto* hdrView = static_cast<::Diligent::ITextureView*>(hdrShaderResource_);
        static_cast<::Diligent::IShaderResourceVariable*>(toneMapTextureVariable_)->Set(hdrView);
        static_cast<::Diligent::IShaderResourceVariable*>(toneMapBloomVariable_)->Set(
            static_cast<::Diligent::ITextureView*>(bloomOutputView));
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(toneMapBinding_),
                                       ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ::Diligent::DrawAttribs draw;
        draw.NumVertices = 4;
        draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw(draw);
        return true;
    });
    void* uiStrongOutputView = bloomShaderResource_;
    const auto uiDownsampleStrong = graph.AddPass("vulkan-ui-downsample-strong", [&] {
        if (!uiBlurEnabled_) return true;
        const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
            1.0f / static_cast<float>(description.Width),
            1.0f / static_cast<float>(description.Height)}, 0.0f, 0.0f};
        UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
        auto* renderTargetView = static_cast<::Diligent::ITextureView*>(bloomRenderTarget_);
        context->SetRenderTargets(1, &renderTargetView, nullptr,
                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomDownsamplePipeline_));
        static_cast<::Diligent::IShaderResourceVariable*>(bloomDownsampleTextureVariable_)->Set(
            static_cast<::Diligent::ITextureView*>(uiSceneShaderResource_));
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomDownsampleBinding_),
                                       ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ::Diligent::DrawAttribs draw;
        draw.NumVertices = 4;
        draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw(draw);
        uiStrongOutputView = bloomShaderResource_;
        return true;
    });
    constexpr std::array<float, 7> uiStrongOffsets{0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    std::vector<std::uint32_t> uiStrongBlurPasses;
    uiStrongBlurPasses.reserve(uiStrongOffsets.size());
    const float uiBlurScale = std::clamp(uiBlurStrength_, 0.0f, 5.0f) / 5.0f;
    const auto scaleUiOffset = [uiBlurScale](float base) {
        return uiBlurScale * (base + 0.5f) - 0.5f;
    };
    for (std::uint32_t index = 0; index < uiStrongOffsets.size(); ++index) {
        uiStrongBlurPasses.push_back(graph.AddPass("vulkan-ui-blur-strong-" + std::to_string(index), [&, index] {
            if (!uiBlurEnabled_) return true;
            const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
                1.0f / static_cast<float>(std::max(1u, description.Width / 6u)),
                1.0f / static_cast<float>(std::max(1u, description.Height / 6u))},
                scaleUiOffset(uiStrongOffsets[index]), 0.0f};
            UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
            const bool writeToPing = index % 2 == 0;
            auto* renderTargetView = static_cast<::Diligent::ITextureView*>(
                writeToPing ? bloomPingRenderTarget_ : bloomRenderTarget_);
            void* inputView = writeToPing ? bloomShaderResource_ : bloomPingShaderResource_;
            context->SetRenderTargets(1, &renderTargetView, nullptr,
                                      ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomBlurPipeline_));
            static_cast<::Diligent::IShaderResourceVariable*>(bloomBlurTextureVariable_)->Set(
                static_cast<::Diligent::ITextureView*>(inputView));
            context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomBlurBinding_),
                                           ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            ::Diligent::DrawAttribs draw;
            draw.NumVertices = 4;
            draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
            context->Draw(draw);
            uiStrongOutputView = writeToPing ? bloomPingShaderResource_ : bloomShaderResource_;
            return true;
        }));
    }
    const auto uiDownsampleWeak = graph.AddPass("vulkan-ui-downsample-weak", [&] {
        if (!uiBlurEnabled_) return true;
        const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
            6.0f / static_cast<float>(std::max(1u, description.Width / 6u)),
            6.0f / static_cast<float>(std::max(1u, description.Height / 6u))}, 0.0f, 0.0f};
        UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
        auto* renderTargetView = static_cast<::Diligent::ITextureView*>(uiWeakRenderTarget_);
        context->SetRenderTargets(1, &renderTargetView, nullptr,
                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomDownsamplePipeline_));
        static_cast<::Diligent::IShaderResourceVariable*>(bloomDownsampleTextureVariable_)->Set(
            static_cast<::Diligent::ITextureView*>(uiStrongOutputView));
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomDownsampleBinding_),
                                       ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ::Diligent::DrawAttribs draw;
        draw.NumVertices = 4;
        draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw(draw);
        return true;
    });
    void* uiWeakOutputView = uiWeakShaderResource_;
    constexpr std::array<float, 2> uiWeakOffsets{0.5f, 1.0f};
    std::vector<std::uint32_t> uiWeakBlurPasses;
    uiWeakBlurPasses.reserve(uiWeakOffsets.size());
    for (std::uint32_t index = 0; index < uiWeakOffsets.size(); ++index) {
        uiWeakBlurPasses.push_back(graph.AddPass("vulkan-ui-blur-weak-" + std::to_string(index), [&, index] {
            if (!uiBlurEnabled_) return true;
            const struct BloomConstants { float texelSize[2]; float offset; float threshold; } values{{
                1.0f / static_cast<float>(std::max(1u, description.Width / 12u)),
                1.0f / static_cast<float>(std::max(1u, description.Height / 12u))},
                scaleUiOffset(uiWeakOffsets[index]), 0.0f};
            UpdateBuffer(bloomConstants_, 0, std::as_bytes(std::span{&values, 1}));
            const bool writeToPing = index % 2 == 0;
            auto* renderTargetView = static_cast<::Diligent::ITextureView*>(
                writeToPing ? uiWeakPingRenderTarget_ : uiWeakRenderTarget_);
            void* inputView = writeToPing ? uiWeakShaderResource_ : uiWeakPingShaderResource_;
            context->SetRenderTargets(1, &renderTargetView, nullptr,
                                      ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(bloomBlurPipeline_));
            static_cast<::Diligent::IShaderResourceVariable*>(bloomBlurTextureVariable_)->Set(
                static_cast<::Diligent::ITextureView*>(inputView));
            context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(bloomBlurBinding_),
                                           ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            ::Diligent::DrawAttribs draw;
            draw.NumVertices = 4;
            draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
            context->Draw(draw);
            uiWeakOutputView = writeToPing ? uiWeakPingShaderResource_ : uiWeakShaderResource_;
            return true;
        }));
    }
    const auto acrylic = graph.AddPass("vulkan-acrylic", [&] {
        const bool enabled = uiBlurEnabled_;
        AcrylicConstants values{};
        if (enabled) {
            const float opacity = uiDarkMode_ ? 180.0f / 255.0f : 150.0f / 255.0f;
            const float tint = uiDarkMode_ ? 20.0f / 255.0f : 245.0f / 255.0f;
            values.tint[0] = tint;
            values.tint[1] = tint;
            values.tint[2] = uiDarkMode_ ? 25.0f / 255.0f : 1.0f;
            values.tint[3] = opacity;
            values.params[0] = uiDarkMode_ ? 1.35f : 1.35f;
            values.params[1] = uiDarkMode_ ? 0.35f : 0.35f;
            values.params[2] = uiDarkMode_ ? 1.0f : 0.0f;
            values.params[3] = 1.0f;
        } else {
            values.params[0] = 1.0f;
        }
        UpdateBuffer(acrylicConstants_, 0, std::as_bytes(std::span{&values, 1}));
        auto* renderTargetView = target;
        context->SetRenderTargets(1, &renderTargetView, nullptr,
                                  ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(acrylicPipeline_));
        void* acrylicInput = uiBlurStrength_ < 2.5f ? uiWeakOutputView : uiStrongOutputView;
        static_cast<::Diligent::IShaderResourceVariable*>(acrylicTextureVariable_)->Set(
            static_cast<::Diligent::ITextureView*>(enabled ? acrylicInput : uiSceneShaderResource_));
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(acrylicBinding_),
                                       ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        ::Diligent::DrawAttribs draw;
        draw.NumVertices = 4;
        draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
        context->Draw(draw);
        return true;
    });
    const auto imgui = graph.AddPass("vulkan-imgui", [&] {
        if (imgui_ != nullptr) imgui_->Render(context, target);
        return true;
    });
    const auto capture = graph.AddPass("vulkan-baseline-capture", [&] {
        if (!baselineCaptureRequested_ || baselineCaptured_ || baselineStagingTexture_ == nullptr) return true;
        ::Diligent::CopyTextureAttribs copy;
        copy.pSrcTexture = target->GetTexture();
        copy.SrcTextureTransitionMode = ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        copy.pDstTexture = static_cast<::Diligent::ITexture*>(baselineStagingTexture_);
        copy.DstTextureTransitionMode = ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
        context->CopyTexture(copy);
        return true;
    });
    const auto present = graph.AddPass("vulkan-present", [&] {
        static_cast<void>(Submit(commands));
        swapChain->Present(syncInterval);
        return true;
    });
    graph.Read(simulation, particles, ResourceUsage::ShaderRead);
    graph.Write(simulation, particles, ResourceUsage::ShaderWrite);
    graph.Read(scene, particles, ResourceUsage::ShaderRead);
    graph.Write(scene, hdr, ResourceUsage::RenderTarget);
    graph.Read(downsample, hdr, ResourceUsage::ShaderRead);
    graph.Write(downsample, bloom, ResourceUsage::RenderTarget);
    for (const auto pass : blurPasses) {
        graph.Read(pass, bloom, ResourceUsage::ShaderRead);
        graph.Write(pass, bloom, ResourceUsage::RenderTarget);
    }
    graph.Read(toneMap, hdr, ResourceUsage::ShaderRead);
    graph.Read(toneMap, bloom, ResourceUsage::ShaderRead);
    graph.Write(toneMap, uiScene, ResourceUsage::RenderTarget);
    graph.Read(uiDownsampleStrong, uiScene, ResourceUsage::ShaderRead);
    graph.Write(uiDownsampleStrong, bloom, ResourceUsage::RenderTarget);
    for (const auto pass : uiStrongBlurPasses) {
        graph.Read(pass, bloom, ResourceUsage::ShaderRead);
        graph.Write(pass, bloom, ResourceUsage::RenderTarget);
    }
    graph.Read(uiDownsampleWeak, bloom, ResourceUsage::ShaderRead);
    graph.Write(uiDownsampleWeak, uiWeak, ResourceUsage::RenderTarget);
    for (const auto pass : uiWeakBlurPasses) {
        graph.Read(pass, uiWeak, ResourceUsage::ShaderRead);
        graph.Write(pass, uiWeak, ResourceUsage::RenderTarget);
    }
    graph.Read(acrylic, uiScene, ResourceUsage::ShaderRead);
    graph.Read(acrylic, bloom, ResourceUsage::ShaderRead);
    graph.Read(acrylic, uiWeak, ResourceUsage::ShaderRead);
    graph.Write(acrylic, drawable, ResourceUsage::RenderTarget);
    graph.Read(imgui, drawable, ResourceUsage::RenderTarget);
    graph.Write(imgui, drawable, ResourceUsage::RenderTarget);
    graph.Read(capture, drawable, ResourceUsage::ShaderRead);
    graph.Write(capture, drawable, ResourceUsage::CopySource);
    graph.Read(present, drawable, ResourceUsage::Present);
    const bool executed = graph.Execute();
    if (!executed || !baselineCaptureRequested_ || baselineCaptured_ || baselineStagingTexture_ == nullptr) return executed;
    context->Flush();
    ::Diligent::MappedTextureSubresource mapped;
    context->MapTextureSubresource(static_cast<::Diligent::ITexture*>(baselineStagingTexture_), 0, 0,
                                   ::Diligent::MAP_READ, ::Diligent::MAP_FLAG_NONE, nullptr, mapped);
    baselineCaptured_ = WriteMappedBaseline(mapped, baselineStagingWidth_, baselineStagingHeight_, baselinePath_.c_str());
    context->UnmapTextureSubresource(static_cast<::Diligent::ITexture*>(baselineStagingTexture_), 0, 0);
    return baselineCaptured_;
}

bool DiligentVulkanAdapter::InitializeImGui(void* nativeView, std::string& error) {
    if (device_ == nullptr || swapChain_ == nullptr || nativeView == nullptr) {
        error = "Diligent Vulkan ImGui requires an initialized surface";
        return false;
    }
    imgui_ = std::make_unique<ParticleSaturn::UI::ImGuiDiligent>();
    if (!imgui_->Init(nativeView, Render::Backend::Vulkan,
                      static_cast<::Diligent::IRenderDevice*>(device_),
                      static_cast<::Diligent::ISwapChain*>(swapChain_))) {
        imgui_.reset();
        error = "Diligent Vulkan could not initialize macOS ImGui";
        return false;
    }
    return true;
}

void DiligentVulkanAdapter::BeginImGuiFrame() {
    if (imgui_ != nullptr) imgui_->NewFrame();
}

bool DiligentVulkanAdapter::ImGuiReady() const noexcept {
    return imgui_ != nullptr && imgui_->IsInitialized();
}

bool DiligentVulkanAdapter::DeviceLost() const noexcept {
    return deviceLost_;
}

void DiligentVulkanAdapter::SetAcrylicSettings(bool enabled, float strength, bool darkMode) noexcept {
    uiBlurEnabled_ = enabled;
    uiBlurStrength_ = std::clamp(strength, 0.0f, 5.0f);
    uiDarkMode_ = darkMode;
}

std::string_view DiligentVulkanAdapter::Name() const noexcept {
    return adapterName_;
}

void DiligentVulkanAdapter::SetParticleSettings(std::uint32_t particleCount, bool paused) noexcept {
    const auto clampedCount = std::clamp(particleCount, 1u, MaxParticleCount);
    particleCountDirty_ = particleCountDirty_ || particleCount_ != clampedCount;
    particleCount_ = clampedCount;
    particlePaused_ = paused;
}

void DiligentVulkanAdapter::SetSceneSettings(const App::SceneState& scene,
                                             const App::RenderSettings& render) noexcept {
    sceneTime_ = static_cast<float>(scene.simulationTimeSeconds);
    sceneScale_ = std::clamp(scene.zoom, 0.1f, 10.0f);
    sceneRotationX_ = scene.rotationX;
    sceneRotationY_ = scene.rotationY;
    pixelRatio_ = std::clamp(render.pixelRatio, 0.25f, 1.0f);
    densityCompensation_ = std::clamp(render.densityCompensation, 0.0f, 2.0f);
}

bool DiligentVulkanAdapter::BaselineCaptureRequested() const noexcept {
    return baselineCaptureRequested_ && baselineCaptured_;
}

BufferHandle DiligentVulkanAdapter::CreateBuffer(const BufferDesc& desc, std::span<const std::byte> initialData) {
    if (device_ == nullptr) throw std::logic_error{"Diligent Vulkan device is not initialized"};
    if (desc.size == 0) throw std::invalid_argument{"buffer size must be non-zero"};
    if (initialData.size() > desc.size) throw std::invalid_argument{"initial buffer data exceeds buffer size"};
    if (HasUsage(desc.usage, BufferUsage::Storage) &&
        (desc.elementStride == 0 || desc.size % desc.elementStride != 0)) {
        throw std::invalid_argument{"a storage buffer requires a non-zero element stride that divides its size"};
    }

    ::Diligent::BufferDesc nativeDesc;
    nativeDesc.Name = "ParticleSaturn Vulkan buffer";
    nativeDesc.Size = static_cast<::Diligent::Uint64>(desc.size);
    nativeDesc.Usage = ::Diligent::USAGE_DEFAULT;
    nativeDesc.BindFlags = ToDiligentBindFlags(desc.usage);
    if (HasUsage(desc.usage, BufferUsage::Storage)) {
        nativeDesc.Mode = ::Diligent::BUFFER_MODE_STRUCTURED;
        nativeDesc.ElementByteStride = static_cast<::Diligent::Uint32>(desc.elementStride);
    }
    ::Diligent::BufferData nativeData;
    nativeData.pData = initialData.empty() ? nullptr : initialData.data();
    nativeData.DataSize = static_cast<::Diligent::Uint64>(initialData.size());
    ::Diligent::IBuffer* nativeBuffer = nullptr;
    static_cast<::Diligent::IRenderDevice*>(device_)->CreateBuffer(
        nativeDesc, initialData.empty() ? nullptr : &nativeData, &nativeBuffer);
    if (nativeBuffer == nullptr) throw std::runtime_error{"Diligent Vulkan could not create a buffer"};

    for (std::uint32_t index = 0; index < buffers_.size(); ++index) {
        auto& entry = buffers_[index];
        if (entry.buffer == nullptr && !entry.pendingRelease) {
            entry.buffer = nativeBuffer;
            entry.size = desc.size;
            entry.usage = ResourceUsage::Undefined;
            entry.retireAfter = 0;
            return {index, entry.generation};
        }
    }
    buffers_.push_back({nativeBuffer, desc.size, 1, ResourceUsage::Undefined, 0, false});
    return {static_cast<std::uint32_t>(buffers_.size() - 1), 1};
}

void DiligentVulkanAdapter::UpdateBuffer(BufferHandle buffer, std::size_t offset, std::span<const std::byte> data) {
    if (!commandsOpen_) throw std::logic_error{"begin commands before updating a buffer"};
    auto* nativeBuffer = static_cast<::Diligent::IBuffer*>(ResolveBuffer(buffer));
    auto& entry = buffers_[buffer.index];
    if (data.empty() || offset > entry.size || data.size() > entry.size - offset) {
        throw std::out_of_range{"buffer update range is invalid"};
    }
    static_cast<::Diligent::IDeviceContext*>(context_)->UpdateBuffer(
        nativeBuffer, static_cast<::Diligent::Uint64>(offset), static_cast<::Diligent::Uint64>(data.size()), data.data(),
        ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    entry.usage = ResourceUsage::CopyDestination;
}

void DiligentVulkanAdapter::DestroyBuffer(BufferHandle buffer, FrameToken afterFrame) {
    if (!buffer || buffer.index >= buffers_.size()) {
        throw std::out_of_range{"buffer handle does not belong to the Diligent Vulkan adapter"};
    }
    auto& entry = buffers_[buffer.index];
    if (entry.buffer == nullptr || entry.pendingRelease || entry.generation != buffer.generation) {
        throw std::logic_error{"buffer handle is stale or already scheduled for release"};
    }
    entry.pendingRelease = true;
    entry.retireAfter = afterFrame.value;
    ++entry.generation;
    if (entry.generation == 0) entry.generation = 1;
    ReleaseRetiredBuffers();
}

CommandList& DiligentVulkanAdapter::BeginCommands() {
    if (device_ == nullptr || context_ == nullptr) throw std::logic_error{"Diligent Vulkan device is not initialized"};
    if (commandsOpen_) throw std::logic_error{"a Diligent Vulkan command list is already open"};
    commandsOpen_ = true;
    return *this;
}

FrameToken DiligentVulkanAdapter::Submit(CommandList& commands) {
    if (&commands != static_cast<CommandList*>(this) || !commandsOpen_) {
        throw std::invalid_argument{"command list does not belong to the Diligent Vulkan adapter"};
    }
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    context->Flush();
    context->FinishFrame();
    commandsOpen_ = false;
    ++submissionValue_;
    ReleaseRetiredBuffers();
    return {submissionValue_};
}

void DiligentVulkanAdapter::Transition(BufferHandle buffer, ResourceUsage before, ResourceUsage after) {
    if (!commandsOpen_) throw std::logic_error{"begin commands before recording a resource transition"};
    auto* nativeBuffer = static_cast<::Diligent::IBuffer*>(ResolveBuffer(buffer));
    auto& entry = buffers_[buffer.index];
    if (entry.usage != before) throw std::logic_error{"buffer transition does not match the tracked resource state"};
    ::Diligent::StateTransitionDesc barrier;
    barrier.pResource = nativeBuffer;
    barrier.OldState = ToDiligentResourceState(before);
    barrier.NewState = ToDiligentResourceState(after);
    barrier.TransitionType = ::Diligent::STATE_TRANSITION_TYPE_IMMEDIATE;
    barrier.Flags = ::Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE;
    static_cast<::Diligent::IDeviceContext*>(context_)->TransitionResourceStates(1, &barrier);
    entry.usage = after;
}

void DiligentVulkanAdapter::Transition(TextureHandle, ResourceUsage, ResourceUsage) {
    throw std::logic_error{"Diligent Vulkan texture transitions are not available yet"};
}

void DiligentVulkanAdapter::DrawIndirect(BufferHandle arguments, std::size_t offset) {
    if (!commandsOpen_) throw std::logic_error{"begin commands before recording an indirect draw"};
    if (offset % sizeof(std::uint32_t) != 0) throw std::invalid_argument{"indirect draw offset must be aligned to 4 bytes"};
    auto* nativeBuffer = static_cast<::Diligent::IBuffer*>(ResolveBuffer(arguments));
    if (buffers_[arguments.index].usage != ResourceUsage::IndirectArgument) {
        throw std::logic_error{"indirect draw arguments must be transitioned to the indirect-argument state"};
    }
    ::Diligent::DrawIndirectAttribs draw;
    draw.pAttribsBuffer = nativeBuffer;
    draw.DrawArgsOffset = static_cast<::Diligent::Uint64>(offset);
    draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
    draw.AttribsBufferStateTransitionMode = ::Diligent::RESOURCE_STATE_TRANSITION_MODE_NONE;
    static_cast<::Diligent::IDeviceContext*>(context_)->DrawIndirect(draw);
}

void DiligentVulkanAdapter::Dispatch(std::uint32_t, std::uint32_t, std::uint32_t) {
    throw std::logic_error{"Diligent Vulkan dispatch is not available through the shared device yet"};
}

void* DiligentVulkanAdapter::ResolveBuffer(BufferHandle buffer) const {
    if (!buffer || buffer.index >= buffers_.size()) {
        throw std::out_of_range{"buffer handle does not belong to the Diligent Vulkan adapter"};
    }
    const auto& entry = buffers_[buffer.index];
    if (entry.buffer == nullptr || entry.pendingRelease || entry.generation != buffer.generation) {
        throw std::logic_error{"buffer handle is stale or already scheduled for release"};
    }
    return entry.buffer;
}

void DiligentVulkanAdapter::ReleaseRetiredBuffers() noexcept {
    for (auto& entry : buffers_) {
        if (entry.pendingRelease && entry.retireAfter <= submissionValue_) {
            static_cast<::Diligent::IBuffer*>(entry.buffer)->Release();
            entry.buffer = nullptr;
            entry.size = 0;
            entry.usage = ResourceUsage::Undefined;
            entry.retireAfter = 0;
            entry.pendingRelease = false;
        }
    }
}

bool DiligentVulkanAdapter::CreateHdrTargets(std::uint32_t width, std::uint32_t height, std::string& error) {
    if (device_ == nullptr || width == 0 || height == 0) return false;
    if (baselineStagingTexture_ != nullptr) {
        static_cast<::Diligent::ITexture*>(baselineStagingTexture_)->Release();
        baselineStagingTexture_ = nullptr;
        baselineStagingWidth_ = 0;
        baselineStagingHeight_ = 0;
    }
    if (hdrTexture_ != nullptr) {
        static_cast<::Diligent::ITexture*>(hdrTexture_)->Release();
        hdrTexture_ = nullptr;
        hdrRenderTarget_ = nullptr;
        hdrShaderResource_ = nullptr;
    }
    if (uiSceneTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiSceneTexture_)->Release();
    if (uiWeakTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiWeakTexture_)->Release();
    if (uiWeakPingTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiWeakPingTexture_)->Release();
    uiSceneTexture_ = nullptr;
    uiSceneRenderTarget_ = nullptr;
    uiSceneShaderResource_ = nullptr;
    uiWeakTexture_ = nullptr;
    uiWeakRenderTarget_ = nullptr;
    uiWeakShaderResource_ = nullptr;
    uiWeakPingTexture_ = nullptr;
    uiWeakPingRenderTarget_ = nullptr;
    uiWeakPingShaderResource_ = nullptr;
    if (bloomTexture_ != nullptr) static_cast<::Diligent::ITexture*>(bloomTexture_)->Release();
    if (bloomPingTexture_ != nullptr) static_cast<::Diligent::ITexture*>(bloomPingTexture_)->Release();
    bloomTexture_ = nullptr;
    bloomPingTexture_ = nullptr;
    bloomRenderTarget_ = nullptr;
    bloomShaderResource_ = nullptr;
    bloomPingRenderTarget_ = nullptr;
    bloomPingShaderResource_ = nullptr;
    ::Diligent::TextureDesc description;
    description.Name = "ParticleSaturn Vulkan HDR scene";
    description.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
    description.Width = width;
    description.Height = height;
    description.Format = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
    description.BindFlags = ::Diligent::BIND_RENDER_TARGET | ::Diligent::BIND_SHADER_RESOURCE;
    ::Diligent::ITexture* texture = nullptr;
    static_cast<::Diligent::IRenderDevice*>(device_)->CreateTexture(description, nullptr, &texture);
    if (texture == nullptr) {
        error = "Diligent Vulkan could not create the HDR scene target";
        return false;
    }
    hdrTexture_ = texture;
    hdrRenderTarget_ = texture->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
    hdrShaderResource_ = texture->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (hdrRenderTarget_ == nullptr || hdrShaderResource_ == nullptr) {
        texture->Release();
        hdrTexture_ = nullptr;
        hdrRenderTarget_ = nullptr;
        hdrShaderResource_ = nullptr;
        error = "Diligent Vulkan could not create HDR scene views";
        return false;
    }
    ::Diligent::TextureDesc uiDescription = description;
    uiDescription.Name = "ParticleSaturn Vulkan UI scene";
    uiDescription.Format = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
    ::Diligent::ITexture* uiScene = nullptr;
    static_cast<::Diligent::IRenderDevice*>(device_)->CreateTexture(uiDescription, nullptr, &uiScene);
    if (uiScene == nullptr) {
        error = "Diligent Vulkan could not create the UI scene target";
        return false;
    }
    uiSceneTexture_ = uiScene;
    uiSceneRenderTarget_ = uiScene->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
    uiSceneShaderResource_ = uiScene->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
    if (uiSceneRenderTarget_ == nullptr || uiSceneShaderResource_ == nullptr) {
        uiScene->Release();
        uiSceneTexture_ = nullptr;
        uiSceneRenderTarget_ = nullptr;
        uiSceneShaderResource_ = nullptr;
        error = "Diligent Vulkan could not create UI scene views";
        return false;
    }
    if (baselineCaptureRequested_) {
        ::Diligent::TextureDesc stagingDescription;
        stagingDescription.Name = "ParticleSaturn Vulkan baseline staging";
        stagingDescription.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        stagingDescription.Width = width;
        stagingDescription.Height = height;
        stagingDescription.Format = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
        stagingDescription.Usage = ::Diligent::USAGE_STAGING;
        stagingDescription.CPUAccessFlags = ::Diligent::CPU_ACCESS_READ;
        ::Diligent::ITexture* staging = nullptr;
        static_cast<::Diligent::IRenderDevice*>(device_)->CreateTexture(stagingDescription, nullptr, &staging);
        if (staging == nullptr) {
            error = "Diligent Vulkan could not create baseline staging texture";
            return false;
        }
        baselineStagingTexture_ = staging;
        baselineStagingWidth_ = width;
        baselineStagingHeight_ = height;
    }
    const auto bloomWidth = std::max(1u, width / 6u);
    const auto bloomHeight = std::max(1u, height / 6u);
    auto createBloomTexture = [&](const char* name, void*& textureSlot, void*& renderTargetSlot,
                                  void*& shaderResourceSlot) {
        ::Diligent::TextureDesc bloomDescription;
        bloomDescription.Name = name;
        bloomDescription.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        bloomDescription.Width = bloomWidth;
        bloomDescription.Height = bloomHeight;
        bloomDescription.Format = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
        bloomDescription.BindFlags = ::Diligent::BIND_RENDER_TARGET | ::Diligent::BIND_SHADER_RESOURCE;
        ::Diligent::ITexture* bloomTexture = nullptr;
        static_cast<::Diligent::IRenderDevice*>(device_)->CreateTexture(bloomDescription, nullptr, &bloomTexture);
        if (bloomTexture == nullptr) return false;
        textureSlot = bloomTexture;
        renderTargetSlot = bloomTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
        shaderResourceSlot = bloomTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        return renderTargetSlot != nullptr && shaderResourceSlot != nullptr;
    };
    if (!createBloomTexture("ParticleSaturn Vulkan Bloom", bloomTexture_, bloomRenderTarget_, bloomShaderResource_) ||
        !createBloomTexture("ParticleSaturn Vulkan Bloom Ping Pong", bloomPingTexture_, bloomPingRenderTarget_, bloomPingShaderResource_)) {
        error = "Diligent Vulkan could not create Bloom targets";
        return false;
    }
    const auto weakWidth = std::max(1u, width / 12u);
    const auto weakHeight = std::max(1u, height / 12u);
    auto createWeakTexture = [&](const char* name, void*& textureSlot, void*& renderTargetSlot,
                                 void*& shaderResourceSlot) {
        ::Diligent::TextureDesc weakDescription;
        weakDescription.Name = name;
        weakDescription.Type = ::Diligent::RESOURCE_DIM_TEX_2D;
        weakDescription.Width = weakWidth;
        weakDescription.Height = weakHeight;
        weakDescription.Format = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
        weakDescription.BindFlags = ::Diligent::BIND_RENDER_TARGET | ::Diligent::BIND_SHADER_RESOURCE;
        ::Diligent::ITexture* weakTexture = nullptr;
        static_cast<::Diligent::IRenderDevice*>(device_)->CreateTexture(weakDescription, nullptr, &weakTexture);
        if (weakTexture == nullptr) return false;
        textureSlot = weakTexture;
        renderTargetSlot = weakTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_RENDER_TARGET);
        shaderResourceSlot = weakTexture->GetDefaultView(::Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        return renderTargetSlot != nullptr && shaderResourceSlot != nullptr;
    };
    if (!createWeakTexture("ParticleSaturn Vulkan UI Weak", uiWeakTexture_, uiWeakRenderTarget_, uiWeakShaderResource_) ||
        !createWeakTexture("ParticleSaturn Vulkan UI Weak Ping Pong", uiWeakPingTexture_, uiWeakPingRenderTarget_,
                           uiWeakPingShaderResource_)) {
        error = "Diligent Vulkan could not create UI blur targets";
        return false;
    }
    return true;
}

bool DiligentVulkanAdapter::CreateToneMapPipeline(std::string& error) {
    if (toneMapPipeline_ != nullptr) return true;
    const auto sources = Render::GetFullscreenQuadShaderSources(Render::Backend::Vulkan);
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::ShaderCreateInfo shader{};
    shader.SourceLanguage = sources.Language;
    shader.EntryPoint = "main";
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_VERTEX;
    shader.Desc.Name = "ParticleSaturn Vulkan Tone Map VS";
    shader.Source = sources.Vertex;
    ::Diligent::IShader* vertex = nullptr;
    device->CreateShader(shader, &vertex);
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_PIXEL;
    shader.Desc.Name = "ParticleSaturn Vulkan Tone Map PS";
    shader.Source = sources.Fragment;
    ::Diligent::IShader* fragment = nullptr;
    device->CreateShader(shader, &fragment);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) vertex->Release();
        if (fragment != nullptr) fragment->Release();
        error = "Diligent Vulkan could not compile tone map shaders";
        return false;
    }
    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_PIXEL, "g_Texture", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_PIXEL, "g_BloomTexture", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_PIXEL, "BloomCB", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    ::Diligent::SamplerDesc sampler;
    sampler.MinFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MagFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.AddressU = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    const ::Diligent::ImmutableSamplerDesc immutableSamplers[] = {
        {::Diligent::SHADER_TYPE_PIXEL, "g_Texture", sampler},
        {::Diligent::SHADER_TYPE_PIXEL, "g_BloomTexture", sampler},
    };
    ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Tone Map";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
    pipeline.PSODesc.ResourceLayout.ImmutableSamplers = immutableSamplers;
    pipeline.PSODesc.ResourceLayout.NumImmutableSamplers = static_cast<::Diligent::Uint32>(std::size(immutableSamplers));
    pipeline.GraphicsPipeline.NumRenderTargets = 1;
    pipeline.GraphicsPipeline.RTVFormats[0] = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
    pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
    pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
    pipeline.pVS = vertex;
    pipeline.pPS = fragment;
    ::Diligent::IPipelineState* state = nullptr;
    device->CreateGraphicsPipelineState(pipeline, &state);
    vertex->Release();
    fragment->Release();
    if (state == nullptr) { error = "Diligent Vulkan could not create tone map pipeline"; return false; }
    auto* constants = state->GetStaticVariableByName(::Diligent::SHADER_TYPE_PIXEL, "BloomCB");
    if (constants == nullptr) { state->Release(); error = "Diligent Vulkan tone map constants unavailable"; return false; }
    const ToneMapConstants values{0.5f, 0.0f, {0.0f, 0.0f}};
    toneMapConstants_ = CreateBuffer({sizeof(values), 0, BufferUsage::Uniform}, std::as_bytes(std::span{&values, 1}));
    constants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(toneMapConstants_)));
    ::Diligent::IShaderResourceBinding* binding = nullptr;
    state->CreateShaderResourceBinding(&binding, true);
    auto* texture = binding == nullptr ? nullptr : binding->GetVariableByName(::Diligent::SHADER_TYPE_PIXEL, "g_Texture");
    auto* bloom = binding == nullptr ? nullptr : binding->GetVariableByName(::Diligent::SHADER_TYPE_PIXEL, "g_BloomTexture");
    if (binding == nullptr || texture == nullptr || bloom == nullptr) {
        if (binding != nullptr) binding->Release();
        state->Release();
        error = "Diligent Vulkan tone map bindings unavailable";
        return false;
    }
    toneMapPipeline_ = state;
    toneMapBinding_ = binding;
    toneMapTextureVariable_ = texture;
    toneMapBloomVariable_ = bloom;
    return true;
}

bool DiligentVulkanAdapter::CreateBloomPipelines(std::string& error) {
    if (bloomDownsamplePipeline_ != nullptr && bloomBlurPipeline_ != nullptr) return true;
    const auto& bloomDescription = static_cast<::Diligent::ITexture*>(bloomTexture_)->GetDesc();
    const struct BloomConstants {
        float texelSize[2];
        float offset;
        float threshold;
    } values{{1.0f / bloomDescription.Width, 1.0f / bloomDescription.Height}, 0.0f, 1.0f};
    bloomConstants_ = CreateBuffer({sizeof(values), 0, BufferUsage::Uniform}, std::as_bytes(std::span{&values, 1}));
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::SamplerDesc sampler;
    sampler.MinFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MagFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.AddressU = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    const ::Diligent::ImmutableSamplerDesc immutableSampler{::Diligent::SHADER_TYPE_PIXEL, "g_Texture", sampler};
    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_PIXEL, "g_Texture", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_PIXEL, "BlurCB", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    auto createPipeline = [&](const Render::ShaderSources& sources, const char* name, void*& pipelineSlot,
                              void*& bindingSlot, void*& textureVariableSlot) {
        ::Diligent::ShaderCreateInfo shader{};
        shader.SourceLanguage = sources.Language;
        shader.EntryPoint = "main";
        shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_VERTEX;
        shader.Desc.Name = name;
        shader.Source = sources.Vertex;
        ::Diligent::IShader* vertex = nullptr;
        device->CreateShader(shader, &vertex);
        shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_PIXEL;
        shader.Source = sources.Fragment;
        ::Diligent::IShader* fragment = nullptr;
        device->CreateShader(shader, &fragment);
        if (vertex == nullptr || fragment == nullptr) {
            if (vertex != nullptr) vertex->Release();
            if (fragment != nullptr) fragment->Release();
            return false;
        }
        ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
        pipeline.PSODesc.Name = name;
        pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
        pipeline.PSODesc.ResourceLayout.Variables = variables;
        pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
        pipeline.PSODesc.ResourceLayout.ImmutableSamplers = &immutableSampler;
        pipeline.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
        pipeline.GraphicsPipeline.NumRenderTargets = 1;
        pipeline.GraphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
        pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
        pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
        pipeline.pVS = vertex;
        pipeline.pPS = fragment;
        ::Diligent::IPipelineState* state = nullptr;
        device->CreateGraphicsPipelineState(pipeline, &state);
        vertex->Release();
        fragment->Release();
        if (state == nullptr) return false;
        auto* constants = state->GetStaticVariableByName(::Diligent::SHADER_TYPE_PIXEL, "BlurCB");
        if (constants == nullptr) { state->Release(); return false; }
        constants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(bloomConstants_)));
        ::Diligent::IShaderResourceBinding* binding = nullptr;
        state->CreateShaderResourceBinding(&binding, true);
        auto* texture = binding == nullptr ? nullptr : binding->GetVariableByName(::Diligent::SHADER_TYPE_PIXEL, "g_Texture");
        if (binding == nullptr || texture == nullptr) {
            if (binding != nullptr) binding->Release();
            state->Release();
            return false;
        }
        pipelineSlot = state;
        bindingSlot = binding;
        textureVariableSlot = texture;
        return true;
    };
    if (!createPipeline(Render::GetBloomDownsampleShaderSources(Render::Backend::Vulkan),
                        "ParticleSaturn Vulkan Bloom Downsample", bloomDownsamplePipeline_, bloomDownsampleBinding_,
                        bloomDownsampleTextureVariable_) ||
        !createPipeline(Render::GetBloomBlurShaderSources(Render::Backend::Vulkan),
                        "ParticleSaturn Vulkan Bloom Blur", bloomBlurPipeline_, bloomBlurBinding_, bloomBlurTextureVariable_)) {
        error = "Diligent Vulkan could not create Bloom pipelines";
        return false;
    }
    return true;
}

bool DiligentVulkanAdapter::CreateAcrylicPipeline(std::string& error) {
    if (acrylicPipeline_ != nullptr) return true;
    const auto sources = Render::GetAcrylicCompositeShaderSources(Render::Backend::Vulkan);
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::ShaderCreateInfo shader{};
    shader.SourceLanguage = sources.Language;
    shader.EntryPoint = "main";
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_VERTEX;
    shader.Desc.Name = "ParticleSaturn Vulkan Acrylic VS";
    shader.Source = sources.Vertex;
    ::Diligent::IShader* vertex = nullptr;
    device->CreateShader(shader, &vertex);
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_PIXEL;
    shader.Desc.Name = "ParticleSaturn Vulkan Acrylic PS";
    shader.Source = sources.Fragment;
    ::Diligent::IShader* fragment = nullptr;
    device->CreateShader(shader, &fragment);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) vertex->Release();
        if (fragment != nullptr) fragment->Release();
        error = "Diligent Vulkan could not compile Acrylic shaders";
        return false;
    }

    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_PIXEL, "g_Texture", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_PIXEL, "AcrylicCB", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    ::Diligent::SamplerDesc sampler;
    sampler.MinFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MagFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.MipFilter = ::Diligent::FILTER_TYPE_LINEAR;
    sampler.AddressU = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = ::Diligent::TEXTURE_ADDRESS_CLAMP;
    const ::Diligent::ImmutableSamplerDesc immutableSampler{::Diligent::SHADER_TYPE_PIXEL, "g_Texture", sampler};
    ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Acrylic";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
    pipeline.PSODesc.ResourceLayout.ImmutableSamplers = &immutableSampler;
    pipeline.PSODesc.ResourceLayout.NumImmutableSamplers = 1;
    pipeline.GraphicsPipeline.NumRenderTargets = 1;
    pipeline.GraphicsPipeline.RTVFormats[0] = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
    pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
    pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
    auto& blend = pipeline.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blend.BlendEnable = ::Diligent::False;
    pipeline.pVS = vertex;
    pipeline.pPS = fragment;
    ::Diligent::IPipelineState* state = nullptr;
    device->CreateGraphicsPipelineState(pipeline, &state);
    vertex->Release();
    fragment->Release();
    if (state == nullptr) {
        error = "Diligent Vulkan could not create Acrylic pipeline";
        return false;
    }
    const auto constants = AcrylicConstants{{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}};
    try {
        acrylicConstants_ = CreateBuffer({sizeof(constants), 0, BufferUsage::Uniform},
                                          std::as_bytes(std::span{&constants, 1}));
    } catch (const std::exception& exception) {
        state->Release();
        error = exception.what();
        return false;
    }
    auto* staticConstants = state->GetStaticVariableByName(::Diligent::SHADER_TYPE_PIXEL, "AcrylicCB");
    if (staticConstants == nullptr) {
        state->Release();
        error = "Diligent Vulkan Acrylic constants unavailable";
        return false;
    }
    staticConstants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(acrylicConstants_)));
    ::Diligent::IShaderResourceBinding* binding = nullptr;
    state->CreateShaderResourceBinding(&binding, true);
    auto* texture = binding == nullptr ? nullptr : binding->GetVariableByName(::Diligent::SHADER_TYPE_PIXEL, "g_Texture");
    if (binding == nullptr || texture == nullptr) {
        if (binding != nullptr) binding->Release();
        state->Release();
        error = "Diligent Vulkan Acrylic bindings unavailable";
        return false;
    }
    acrylicPipeline_ = state;
    acrylicBinding_ = binding;
    acrylicTextureVariable_ = texture;
    return true;
}

bool DiligentVulkanAdapter::CreateScenePipeline(std::string& error) {
    if (device_ == nullptr || swapChain_ == nullptr) {
        error = "Diligent Vulkan scene pipeline requires a device and swap chain";
        return false;
    }
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::ShaderCreateInfo shader{};
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_VERTEX;
    shader.Desc.Name = "ParticleSaturn Vulkan Scene VS";
    shader.SourceLanguage = ::Diligent::SHADER_SOURCE_LANGUAGE_GLSL;
    shader.EntryPoint = "main";
    shader.Source = SceneVertexShader;
    ::Diligent::IShader* vertex = nullptr;
    device->CreateShader(shader, &vertex);
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_PIXEL;
    shader.Desc.Name = "ParticleSaturn Vulkan Scene PS";
    shader.Source = SceneFragmentShader;
    ::Diligent::IShader* fragment = nullptr;
    device->CreateShader(shader, &fragment);
    if (vertex == nullptr || fragment == nullptr) {
        if (vertex != nullptr) vertex->Release();
        if (fragment != nullptr) fragment->Release();
        error = "Diligent Vulkan could not compile the scene shaders";
        return false;
    }
    ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Scene";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline.GraphicsPipeline.NumRenderTargets = 1;
    pipeline.GraphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
    pipeline.GraphicsPipeline.DSVFormat = ::Diligent::TEX_FORMAT_UNKNOWN;
    pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
    pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
    pipeline.pVS = vertex;
    pipeline.pPS = fragment;
    auto* scenePipeline = static_cast<::Diligent::IPipelineState*>(scenePipeline_);
    if (scenePipeline != nullptr) scenePipeline->Release();
    scenePipeline_ = nullptr;
    device->CreateGraphicsPipelineState(pipeline, &scenePipeline);
    vertex->Release();
    fragment->Release();
    if (scenePipeline == nullptr) {
        error = "Diligent Vulkan could not create the scene pipeline";
        return false;
    }
    scenePipeline_ = scenePipeline;
    return true;
}

bool DiligentVulkanAdapter::CreateParticlePipeline(std::string& error) {
    if (particlePipeline_ != nullptr) return true;
    const std::array<Particle, 3> particles{{
        {{-0.55f, 0.35f, 0.0f, 1.0f}, 0xFFB866FFu, 0.0f, 0, 0},
        {{ 0.10f, 0.62f, 0.0f, 1.0f}, 0xFF70D4FFu, 0.0f, 0, 0},
        {{ 0.58f,-0.28f, 0.0f, 1.0f}, 0xFFFFD080u, 0.0f, 1, 0},
    }};
    try {
        for (auto& particleBuffer : particleBuffers_) {
            particleBuffer = CreateBuffer(
                {sizeof(Particle) * MaxParticleCount, sizeof(Particle), BufferUsage::Storage}, {});
        }
        const ParticleComputeConstants constants{1.0f / 120.0f, 1.0f, 0.0f, MaxParticleCount};
        particleComputeConstants_ = CreateBuffer(
            {sizeof(constants), 0, BufferUsage::Uniform}, std::as_bytes(std::span{&constants, 1}));
        const ParticleRenderConstants renderConstants{{0.0f, 1.0f, 0.4f, 0.0f},
                                                       {1.0f, 1080.0f, 1.0f, 0.6f}};
        particleRenderConstants_ = CreateBuffer(
            {sizeof(renderConstants), 0, BufferUsage::Uniform},
            std::as_bytes(std::span{&renderConstants, 1}));
        const std::array<std::uint32_t, 4> arguments{6, MaxParticleCount, 0, 0};
        particleIndirectArguments_ = CreateBuffer(
            {sizeof(arguments), 0, BufferUsage::Indirect}, std::as_bytes(std::span{arguments}));
        auto& commands = BeginCommands();
        for (const auto particleBuffer : particleBuffers_) {
            commands.Transition(particleBuffer, ResourceUsage::Undefined, ResourceUsage::ShaderWrite);
        }
        commands.Transition(particleIndirectArguments_, ResourceUsage::Undefined, ResourceUsage::IndirectArgument);
        if (!CreateParticleInitializationPipeline(error)) return false;
        auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
        auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
        auto* initState = static_cast<::Diligent::IPipelineState*>(particleInitializationPipeline_);
        auto* initBinding = static_cast<::Diligent::IShaderResourceBinding*>(particleInitializationBinding_);
        auto* initOutput = static_cast<::Diligent::IShaderResourceVariable*>(particleInitializationOutputVariable_);
        auto* initConstants = static_cast<::Diligent::IShaderResourceVariable*>(particleInitializationConstantsVariable_);
        initConstants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleInitializationConstants_)));
        context->SetPipelineState(initState);
        for (const auto particleBuffer : particleBuffers_) {
            initOutput->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleBuffer))->GetDefaultView(::Diligent::BUFFER_VIEW_UNORDERED_ACCESS));
            context->CommitShaderResources(initBinding, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            context->DispatchCompute({(MaxParticleCount + 255) / 256, 1, 1});
            auto& entry = buffers_[particleBuffer.index];
            ::Diligent::StateTransitionDesc barrier;
            barrier.pResource = static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleBuffer));
            barrier.OldState = ::Diligent::RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.NewState = ::Diligent::RESOURCE_STATE_SHADER_RESOURCE;
            barrier.TransitionType = ::Diligent::STATE_TRANSITION_TYPE_IMMEDIATE;
            barrier.Flags = ::Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE;
            context->TransitionResourceStates(1, &barrier);
            entry.usage = ResourceUsage::ShaderRead;
        }
        static_cast<void>(Submit(commands));
    } catch (const std::exception& exception) { error = exception.what(); return false; }
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::ShaderCreateInfo shader{};
    shader.SourceLanguage = ::Diligent::SHADER_SOURCE_LANGUAGE_GLSL;
    shader.EntryPoint = "main";
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_VERTEX;
    shader.Desc.Name = "ParticleSaturn Vulkan Particle VS";
    shader.Source = ParticleVertexShader;
    ::Diligent::IShader* vertex = nullptr;
    device->CreateShader(shader, &vertex);
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_PIXEL;
    shader.Desc.Name = "ParticleSaturn Vulkan Particle PS";
    shader.Source = ParticleFragmentShader;
    ::Diligent::IShader* fragment = nullptr;
    device->CreateShader(shader, &fragment);
    if (vertex == nullptr || fragment == nullptr) { if (vertex) vertex->Release(); if (fragment) fragment->Release(); error = "Diligent Vulkan could not compile particle shaders"; return false; }
    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_VERTEX, "gParticles", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_VERTEX, "RenderConstants", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Particles";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
    pipeline.GraphicsPipeline.NumRenderTargets = 1;
    pipeline.GraphicsPipeline.RTVFormats[0] = ::Diligent::TEX_FORMAT_RGBA16_FLOAT;
    pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
    pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
    auto& blend = pipeline.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blend.BlendEnable = ::Diligent::True;
    blend.SrcBlend = ::Diligent::BLEND_FACTOR_SRC_ALPHA;
    blend.DestBlend = ::Diligent::BLEND_FACTOR_ONE;
    blend.BlendOp = ::Diligent::BLEND_OPERATION_ADD;
    blend.SrcBlendAlpha = ::Diligent::BLEND_FACTOR_ONE;
    blend.DestBlendAlpha = ::Diligent::BLEND_FACTOR_ONE;
    blend.BlendOpAlpha = ::Diligent::BLEND_OPERATION_ADD;
    pipeline.pVS = vertex; pipeline.pPS = fragment;
    ::Diligent::IPipelineState* state = nullptr; device->CreateGraphicsPipelineState(pipeline, &state); vertex->Release(); fragment->Release();
    if (state == nullptr) { error = "Diligent Vulkan could not create the particle pipeline"; return false; }
    auto* renderConstantsVariable = state->GetStaticVariableByName(::Diligent::SHADER_TYPE_VERTEX, "RenderConstants");
    if (renderConstantsVariable == nullptr) {
        state->Release();
        error = "Diligent Vulkan could not access particle render constants";
        return false;
    }
    renderConstantsVariable->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleRenderConstants_)));
    ::Diligent::IShaderResourceBinding* binding = nullptr; state->CreateShaderResourceBinding(&binding, true);
    if (binding == nullptr) { state->Release(); error = "Diligent Vulkan could not create particle bindings"; return false; }
    auto* variable = binding->GetVariableByName(::Diligent::SHADER_TYPE_VERTEX, "gParticles");
    if (variable == nullptr) { binding->Release(); state->Release(); error = "Diligent Vulkan could not access particle bindings"; return false; }
    particlePipeline_ = state;
    particleBinding_ = binding;
    particleRenderVariable_ = variable;
    return CreateParticleComputePipeline(error);
}

bool DiligentVulkanAdapter::CreateParticleComputePipeline(std::string& error) {
    auto* device = static_cast<::Diligent::IRenderDevice*>(device_);
    ::Diligent::ShaderCreateInfo shader{};
    shader.SourceLanguage = ::Diligent::SHADER_SOURCE_LANGUAGE_GLSL;
    shader.EntryPoint = "main";
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_COMPUTE;
    shader.Desc.Name = "ParticleSaturn Vulkan Particle CS";
    shader.Source = ParticleComputeShader;
    ::Diligent::IShader* compute = nullptr;
    device->CreateShader(shader, &compute);
    if (compute == nullptr) {
        error = "Diligent Vulkan could not compile the particle compute shader";
        return false;
    }
    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_COMPUTE, "gParticlesIn", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_COMPUTE, "gParticlesOut", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_COMPUTE, "ComputeConstants", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    ::Diligent::ComputePipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Particle Simulation";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_COMPUTE;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
    pipeline.pCS = compute;
    ::Diligent::IPipelineState* state = nullptr;
    device->CreateComputePipelineState(pipeline, &state);
    compute->Release();
    if (state == nullptr) {
        error = "Diligent Vulkan could not create the particle compute pipeline";
        return false;
    }
    ::Diligent::IShaderResourceBinding* binding = nullptr;
    state->CreateShaderResourceBinding(&binding, true);
    if (binding == nullptr) {
        state->Release();
        error = "Diligent Vulkan could not create particle compute bindings";
        return false;
    }
    auto* input = binding->GetVariableByName(::Diligent::SHADER_TYPE_COMPUTE, "gParticlesIn");
    auto* output = binding->GetVariableByName(::Diligent::SHADER_TYPE_COMPUTE, "gParticlesOut");
    auto* constants = binding->GetVariableByName(::Diligent::SHADER_TYPE_COMPUTE, "ComputeConstants");
    if (input == nullptr || output == nullptr || constants == nullptr) {
        binding->Release();
        state->Release();
        error = "Diligent Vulkan could not access particle compute bindings";
        return false;
    }
    constants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleComputeConstants_)));
    particleComputePipeline_ = state;
    particleComputeBinding_ = binding;
    particleComputeInputVariable_ = input;
    particleComputeOutputVariable_ = output;
    return true;
}

bool DiligentVulkanAdapter::CreateParticleInitializationPipeline(std::string& error) {
    if (particleInitializationPipeline_ != nullptr) return true;
    const auto source = Render::GetSaturnInitComputeShaderSource(Render::Backend::Vulkan);
    ::Diligent::ShaderCreateInfo shader{};
    shader.SourceLanguage = source.Language;
    shader.EntryPoint = "main";
    shader.Desc.ShaderType = ::Diligent::SHADER_TYPE_COMPUTE;
    shader.Desc.Name = "ParticleSaturn Vulkan Particle Initialization CS";
    shader.Source = source.Source;
    ::Diligent::IShader* compute = nullptr;
    static_cast<::Diligent::IRenderDevice*>(device_)->CreateShader(shader, &compute);
    if (compute == nullptr) { error = "Diligent Vulkan could not compile particle initialization shader"; return false; }
    const ::Diligent::ShaderResourceVariableDesc variables[] = {
        {::Diligent::SHADER_TYPE_COMPUTE, "g_ParticlesOut", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {::Diligent::SHADER_TYPE_COMPUTE, "InitConstants", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    ::Diligent::ComputePipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Particle Initialization";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_COMPUTE;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = static_cast<::Diligent::Uint32>(std::size(variables));
    pipeline.pCS = compute;
    ::Diligent::IPipelineState* state = nullptr;
    static_cast<::Diligent::IRenderDevice*>(device_)->CreateComputePipelineState(pipeline, &state);
    compute->Release();
    if (state == nullptr) { error = "Diligent Vulkan could not create particle initialization pipeline"; return false; }
    auto* constants = state->GetStaticVariableByName(::Diligent::SHADER_TYPE_COMPUTE, "InitConstants");
    if (constants == nullptr) { state->Release(); error = "Diligent Vulkan initialization constants unavailable"; return false; }
    const ParticleInitializationConstants values{MaxParticleCount, 0x53415455u, 18.0f, 0.0f};
    particleInitializationConstants_ = CreateBuffer({sizeof(values), 0, BufferUsage::Uniform}, std::as_bytes(std::span{&values, 1}));
    constants->Set(static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleInitializationConstants_)));
    ::Diligent::IShaderResourceBinding* binding = nullptr;
    state->CreateShaderResourceBinding(&binding, true);
    auto* output = binding == nullptr ? nullptr : binding->GetVariableByName(::Diligent::SHADER_TYPE_COMPUTE, "g_ParticlesOut");
    if (binding == nullptr || output == nullptr) {
        if (binding != nullptr) binding->Release(); state->Release(); error = "Diligent Vulkan particle initialization bindings unavailable"; return false;
    }
    particleInitializationPipeline_ = state;
    particleInitializationBinding_ = binding;
    particleInitializationOutputVariable_ = output;
    particleInitializationConstantsVariable_ = constants;
    return true;
}

bool DiligentVulkanAdapter::SimulateParticles(CommandList& commands) {
    const auto input = particleBuffers_[particleReadIndex_];
    const auto output = particleBuffers_[particleWriteIndex_];
    if (!input || !output || !particleComputeConstants_) return false;
    if (particleCountDirty_) {
        UpdateBuffer(particleIndirectArguments_, sizeof(std::uint32_t), std::as_bytes(std::span{&particleCount_, 1}));
        commands.Transition(particleIndirectArguments_, ResourceUsage::CopyDestination,
                            ResourceUsage::IndirectArgument);
        particleCountDirty_ = false;
    }
    if (particlePaused_) return true;
    const ParticleComputeConstants constants{1.0f / 120.0f, 1.0f, 0.0f, particleCount_};
    UpdateBuffer(particleComputeConstants_, 0, std::as_bytes(std::span{&constants, 1}));
    const auto outputUsage = buffers_[output.index].usage;
    commands.Transition(output, outputUsage, ResourceUsage::ShaderWrite);
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    static_cast<::Diligent::IShaderResourceVariable*>(particleComputeInputVariable_)->Set(
        static_cast<::Diligent::IBuffer*>(ResolveBuffer(input))->GetDefaultView(::Diligent::BUFFER_VIEW_SHADER_RESOURCE));
    static_cast<::Diligent::IShaderResourceVariable*>(particleComputeOutputVariable_)->Set(
        static_cast<::Diligent::IBuffer*>(ResolveBuffer(output))->GetDefaultView(::Diligent::BUFFER_VIEW_UNORDERED_ACCESS));
    context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(particleComputePipeline_));
    context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(particleComputeBinding_), ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ::Diligent::DispatchComputeAttribs dispatch{};
    dispatch.ThreadGroupCountX = 1;
    dispatch.ThreadGroupCountY = 1;
    dispatch.ThreadGroupCountZ = 1;
    context->DispatchCompute(dispatch);
    commands.Transition(output, ResourceUsage::ShaderWrite, ResourceUsage::ShaderRead);
    const auto previousRender = particleRenderIndex_;
    particleRenderIndex_ = particleReadIndex_;
    particleReadIndex_ = particleWriteIndex_;
    particleWriteIndex_ = previousRender;
    return true;
}

void DiligentVulkanAdapter::Shutdown() noexcept {
    commandsOpen_ = false;
    if (context_ != nullptr) {
        static_cast<::Diligent::IDeviceContext*>(context_)->Flush();
    }
    imgui_.reset();
    if (swapChain_ != nullptr) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
    }
    if (scenePipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(scenePipeline_)->Release();
        scenePipeline_ = nullptr;
    }
    if (toneMapBinding_ != nullptr) {
        static_cast<::Diligent::IShaderResourceBinding*>(toneMapBinding_)->Release();
        toneMapBinding_ = nullptr;
    }
    if (toneMapPipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(toneMapPipeline_)->Release();
        toneMapPipeline_ = nullptr;
    }
    toneMapTextureVariable_ = nullptr;
    toneMapBloomVariable_ = nullptr;
    if (hdrTexture_ != nullptr) {
        static_cast<::Diligent::ITexture*>(hdrTexture_)->Release();
        hdrTexture_ = nullptr;
        hdrRenderTarget_ = nullptr;
        hdrShaderResource_ = nullptr;
    }
    if (baselineStagingTexture_ != nullptr) {
        static_cast<::Diligent::ITexture*>(baselineStagingTexture_)->Release();
        baselineStagingTexture_ = nullptr;
    }
    baselineStagingWidth_ = 0;
    baselineStagingHeight_ = 0;
    if (uiSceneTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiSceneTexture_)->Release();
    if (uiWeakTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiWeakTexture_)->Release();
    if (uiWeakPingTexture_ != nullptr) static_cast<::Diligent::ITexture*>(uiWeakPingTexture_)->Release();
    uiSceneTexture_ = nullptr;
    uiSceneRenderTarget_ = nullptr;
    uiSceneShaderResource_ = nullptr;
    uiWeakTexture_ = nullptr;
    uiWeakRenderTarget_ = nullptr;
    uiWeakShaderResource_ = nullptr;
    uiWeakPingTexture_ = nullptr;
    uiWeakPingRenderTarget_ = nullptr;
    uiWeakPingShaderResource_ = nullptr;
    if (bloomDownsampleBinding_ != nullptr) {
        static_cast<::Diligent::IShaderResourceBinding*>(bloomDownsampleBinding_)->Release();
        bloomDownsampleBinding_ = nullptr;
    }
    if (bloomBlurBinding_ != nullptr) {
        static_cast<::Diligent::IShaderResourceBinding*>(bloomBlurBinding_)->Release();
        bloomBlurBinding_ = nullptr;
    }
    if (bloomDownsamplePipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(bloomDownsamplePipeline_)->Release();
        bloomDownsamplePipeline_ = nullptr;
    }
    if (bloomBlurPipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(bloomBlurPipeline_)->Release();
        bloomBlurPipeline_ = nullptr;
    }
    if (acrylicBinding_ != nullptr) {
        static_cast<::Diligent::IShaderResourceBinding*>(acrylicBinding_)->Release();
        acrylicBinding_ = nullptr;
    }
    if (acrylicPipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(acrylicPipeline_)->Release();
        acrylicPipeline_ = nullptr;
    }
    bloomDownsampleTextureVariable_ = nullptr;
    bloomBlurTextureVariable_ = nullptr;
    if (bloomTexture_ != nullptr) static_cast<::Diligent::ITexture*>(bloomTexture_)->Release();
    if (bloomPingTexture_ != nullptr) static_cast<::Diligent::ITexture*>(bloomPingTexture_)->Release();
    bloomTexture_ = nullptr;
    bloomPingTexture_ = nullptr;
    bloomRenderTarget_ = nullptr;
    bloomShaderResource_ = nullptr;
    bloomPingRenderTarget_ = nullptr;
    bloomPingShaderResource_ = nullptr;
    particleRenderVariable_ = nullptr;
    if (particleBinding_ != nullptr) { static_cast<::Diligent::IShaderResourceBinding*>(particleBinding_)->Release(); particleBinding_ = nullptr; }
    if (particlePipeline_ != nullptr) { static_cast<::Diligent::IPipelineState*>(particlePipeline_)->Release(); particlePipeline_ = nullptr; }
    particleComputeInputVariable_ = nullptr;
    particleComputeOutputVariable_ = nullptr;
    if (particleComputeBinding_ != nullptr) { static_cast<::Diligent::IShaderResourceBinding*>(particleComputeBinding_)->Release(); particleComputeBinding_ = nullptr; }
    if (particleComputePipeline_ != nullptr) { static_cast<::Diligent::IPipelineState*>(particleComputePipeline_)->Release(); particleComputePipeline_ = nullptr; }
    particleInitializationOutputVariable_ = nullptr;
    particleInitializationConstantsVariable_ = nullptr;
    if (particleInitializationBinding_ != nullptr) { static_cast<::Diligent::IShaderResourceBinding*>(particleInitializationBinding_)->Release(); particleInitializationBinding_ = nullptr; }
    if (particleInitializationPipeline_ != nullptr) { static_cast<::Diligent::IPipelineState*>(particleInitializationPipeline_)->Release(); particleInitializationPipeline_ = nullptr; }
    for (auto& entry : buffers_) {
        if (entry.buffer != nullptr) static_cast<::Diligent::IBuffer*>(entry.buffer)->Release();
    }
    buffers_.clear();
    sceneIndirectArguments_ = {};
    toneMapConstants_ = {};
    bloomConstants_ = {};
    acrylicConstants_ = {};
    particleBuffers_[0] = {};
    particleBuffers_[1] = {};
    particleBuffers_[2] = {};
    particleComputeConstants_ = {};
    particleRenderConstants_ = {};
    particleInitializationConstants_ = {};
    particleIndirectArguments_ = {};
    particleRenderIndex_ = 0;
    particleReadIndex_ = 1;
    particleWriteIndex_ = 2;
    particleCount_ = MaxParticleCount;
    particlePaused_ = false;
    particleCountDirty_ = false;
    sceneTime_ = 0.0f;
    sceneScale_ = 1.0f;
    sceneRotationX_ = 0.4f;
    sceneRotationY_ = 0.0f;
    pixelRatio_ = 1.0f;
    densityCompensation_ = 0.6f;
    uiBlurEnabled_ = true;
    uiBlurStrength_ = 2.0f;
    uiDarkMode_ = true;
    if (context_ != nullptr) {
        static_cast<::Diligent::IDeviceContext*>(context_)->Release();
        context_ = nullptr;
    }
    if (device_ != nullptr) {
        static_cast<::Diligent::IRenderDevice*>(device_)->Release();
        device_ = nullptr;
    }
    adapterName_.clear();
    capabilities_ = {};
    submissionValue_ = 0;
    deviceLost_ = false;
    presentedFrameCount_ = 0;
    baselineCaptureRequested_ = false;
    baselineCaptured_ = false;
    baselinePath_.clear();
}

const std::string& DiligentVulkanAdapter::AdapterName() const noexcept {
    return adapterName_;
}

const GpuCapabilities& DiligentVulkanAdapter::Capabilities() const noexcept {
    return capabilities_;
}

} // namespace ParticleSaturn::Gpu::Diligent
