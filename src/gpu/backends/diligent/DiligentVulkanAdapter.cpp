#include "DiligentVulkanAdapter.h"

#include "render/RenderGraph.h"
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
#include <stdexcept>
#include <utility>

namespace ParticleSaturn::Gpu::Diligent {
namespace {

bool IsEnabled(::Diligent::DEVICE_FEATURE_STATE state) {
    return state != ::Diligent::DEVICE_FEATURE_STATE_DISABLED;
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
    vec2 position = uv * 2.0 - 1.0;
    position.x *= 1.45;
    const vec3 space = vec3(0.002, 0.003, 0.008);
    vec3 result = space;
    vec2 cell = floor(uv * vec2(170.0, 100.0));
    float star = step(0.997, hash(cell));
    result += vec3(star) * (0.18 + 0.7 * hash(cell + 13.0));
    float planet = length(position);
    if (planet < 0.33) {
        vec3 light = normalize(vec3(-0.45, 0.35, 0.8));
        float depth = sqrt(max(0.0, 0.33 * 0.33 - dot(position, position)));
        vec3 normal = normalize(vec3(position, depth));
        float diffuse = max(0.08, dot(normal, light));
        result = vec3(0.93, 0.75, 0.48) * diffuse;
    }
    float ringRadius = length(vec2(position.x, position.y * 2.7));
    float ring = smoothstep(0.62, 0.59, ringRadius) * smoothstep(0.38, 0.42, ringRadius);
    ring *= step(0.0, abs(position.y) + 0.12);
    result = mix(result, vec3(0.72, 0.60, 0.42), ring * 0.75);
    color = vec4(result, 1.0);
}
)";

constexpr const char* ParticleVertexShader = R"(
struct Particle { vec4 position; uint color; float speed; uint isRing; uint padding; };
layout(set=0, binding=0, std430) readonly buffer gParticles { Particle particles[]; } particleBuffer;
layout(location = 0) out vec4 particleColor;
void main() {
    Particle particle = particleBuffer.particles[gl_VertexIndex];
    gl_Position = particle.position;
    gl_PointSize = 12.0;
    particleColor = vec4(float(particle.color & 255u) / 255.0, float((particle.color >> 8u) & 255u) / 255.0, float((particle.color >> 16u) & 255u) / 255.0, 1.0);
}
)";

constexpr const char* ParticleFragmentShader = R"(
layout(location = 0) in vec4 particleColor;
layout(location = 0) out vec4 color;
void main() { color = particleColor; }
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

static_assert(sizeof(Particle) == 32);
static_assert(sizeof(ParticleComputeConstants) == 16);

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

DiligentVulkanAdapter::~DiligentVulkanAdapter() {
    Shutdown();
}

bool DiligentVulkanAdapter::Initialize(App::VulkanDriver driver, const std::string& bundleResources, std::string& error) {
    Shutdown();
    if (!Services::Vulkan::ConfigureDriver(driver, bundleResources, error)) return false;

    ::Diligent::IEngineFactoryVk* factory = ::Diligent::GetEngineFactoryVk();
    if (factory == nullptr) {
        error = "Diligent Vulkan factory is unavailable";
        return false;
    }
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
    if (!CreateScenePipeline(error)) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
        return false;
    }
    if (!CreateParticlePipeline(error)) return false;
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
    static_cast<::Diligent::ISwapChain*>(swapChain_)->Resize(width, height);
    return true;
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
    auto* swapChain = static_cast<::Diligent::ISwapChain*>(swapChain_);
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    auto* target = swapChain->GetCurrentBackBufferRTV();
    if (target == nullptr || !sceneIndirectArguments_ || !particleIndirectArguments_) return false;
    const auto& description = swapChain->GetDesc();
    auto& commands = BeginCommands();
    Render::RenderGraph graph;
    const auto drawable = graph.AddResource({"vulkan-drawable", {description.Width, description.Height, 1}});
    const auto particles = graph.AddResource({"vulkan-particles", {1, 1, 1}});
    const auto simulation = graph.AddPass("vulkan-particle-simulation", [&] {
        return SimulateParticles(commands);
    });
    const auto scene = graph.AddPass("vulkan-scene", [&] {
        context->SetRenderTargets(1, &target, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(scenePipeline_));
        commands.DrawIndirect(sceneIndirectArguments_, 0);
        context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(particlePipeline_));
        auto* particleView = static_cast<::Diligent::IBuffer*>(ResolveBuffer(particleBuffers_[particleRenderIndex_]))->GetDefaultView(::Diligent::BUFFER_VIEW_SHADER_RESOURCE);
        static_cast<::Diligent::IShaderResourceVariable*>(particleRenderVariable_)->Set(particleView);
        context->CommitShaderResources(static_cast<::Diligent::IShaderResourceBinding*>(particleBinding_), ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        commands.DrawIndirect(particleIndirectArguments_, 0);
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
    graph.Write(scene, drawable, ResourceUsage::RenderTarget);
    graph.Read(present, drawable, ResourceUsage::Present);
    return graph.Execute();
}

std::string_view DiligentVulkanAdapter::Name() const noexcept {
    return adapterName_;
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
    pipeline.GraphicsPipeline.RTVFormats[0] = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
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
                {sizeof(particles), sizeof(Particle), BufferUsage::Storage}, std::as_bytes(std::span{particles}));
        }
        const ParticleComputeConstants constants{1.0f / 120.0f, 1.0f, 0.0f, static_cast<std::uint32_t>(particles.size())};
        particleComputeConstants_ = CreateBuffer(
            {sizeof(constants), 0, BufferUsage::Uniform}, std::as_bytes(std::span{&constants, 1}));
        const std::array<std::uint32_t, 4> arguments{3, 1, 0, 0};
        particleIndirectArguments_ = CreateBuffer(
            {sizeof(arguments), 0, BufferUsage::Indirect}, std::as_bytes(std::span{arguments}));
        auto& commands = BeginCommands();
        for (const auto particleBuffer : particleBuffers_) {
            commands.Transition(particleBuffer, ResourceUsage::Undefined, ResourceUsage::ShaderRead);
        }
        commands.Transition(particleIndirectArguments_, ResourceUsage::Undefined, ResourceUsage::IndirectArgument);
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
    ::Diligent::ShaderResourceVariableDesc variables[] = {{::Diligent::SHADER_TYPE_VERTEX, "gParticles", ::Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC}};
    ::Diligent::GraphicsPipelineStateCreateInfo pipeline{};
    pipeline.PSODesc.Name = "ParticleSaturn Vulkan Particles";
    pipeline.PSODesc.PipelineType = ::Diligent::PIPELINE_TYPE_GRAPHICS;
    pipeline.PSODesc.ResourceLayout.Variables = variables;
    pipeline.PSODesc.ResourceLayout.NumVariables = 1;
    pipeline.GraphicsPipeline.NumRenderTargets = 1;
    pipeline.GraphicsPipeline.RTVFormats[0] = static_cast<::Diligent::ISwapChain*>(swapChain_)->GetDesc().ColorBufferFormat;
    pipeline.GraphicsPipeline.PrimitiveTopology = ::Diligent::PRIMITIVE_TOPOLOGY_POINT_LIST;
    pipeline.GraphicsPipeline.RasterizerDesc.CullMode = ::Diligent::CULL_MODE_NONE;
    pipeline.GraphicsPipeline.DepthStencilDesc.DepthEnable = ::Diligent::False;
    pipeline.pVS = vertex; pipeline.pPS = fragment;
    ::Diligent::IPipelineState* state = nullptr; device->CreateGraphicsPipelineState(pipeline, &state); vertex->Release(); fragment->Release();
    if (state == nullptr) { error = "Diligent Vulkan could not create the particle pipeline"; return false; }
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

bool DiligentVulkanAdapter::SimulateParticles(CommandList& commands) {
    const auto input = particleBuffers_[particleReadIndex_];
    const auto output = particleBuffers_[particleWriteIndex_];
    if (!input || !output || !particleComputeConstants_) return false;
    const ParticleComputeConstants constants{1.0f / 120.0f, 1.0f, 0.0f, 3};
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
    if (swapChain_ != nullptr) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
    }
    if (scenePipeline_ != nullptr) {
        static_cast<::Diligent::IPipelineState*>(scenePipeline_)->Release();
        scenePipeline_ = nullptr;
    }
    particleRenderVariable_ = nullptr;
    if (particleBinding_ != nullptr) { static_cast<::Diligent::IShaderResourceBinding*>(particleBinding_)->Release(); particleBinding_ = nullptr; }
    if (particlePipeline_ != nullptr) { static_cast<::Diligent::IPipelineState*>(particlePipeline_)->Release(); particlePipeline_ = nullptr; }
    particleComputeInputVariable_ = nullptr;
    particleComputeOutputVariable_ = nullptr;
    if (particleComputeBinding_ != nullptr) { static_cast<::Diligent::IShaderResourceBinding*>(particleComputeBinding_)->Release(); particleComputeBinding_ = nullptr; }
    if (particleComputePipeline_ != nullptr) { static_cast<::Diligent::IPipelineState*>(particleComputePipeline_)->Release(); particleComputePipeline_ = nullptr; }
    for (auto& entry : buffers_) {
        if (entry.buffer != nullptr) static_cast<::Diligent::IBuffer*>(entry.buffer)->Release();
    }
    buffers_.clear();
    sceneIndirectArguments_ = {};
    particleBuffers_[0] = {};
    particleBuffers_[1] = {};
    particleBuffers_[2] = {};
    particleComputeConstants_ = {};
    particleIndirectArguments_ = {};
    particleRenderIndex_ = 0;
    particleReadIndex_ = 1;
    particleWriteIndex_ = 2;
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
}

const std::string& DiligentVulkanAdapter::AdapterName() const noexcept {
    return adapterName_;
}

const GpuCapabilities& DiligentVulkanAdapter::Capabilities() const noexcept {
    return capabilities_;
}

} // namespace ParticleSaturn::Gpu::Diligent
