#include "DiligentVulkanAdapter.h"

#include "services/vulkan/VulkanDriverRuntime.h"

#include <EngineFactoryVk.h>
#include <RenderDevice.h>
#include <DeviceContext.h>
#include <PipelineState.h>
#include <Shader.h>
#include <SwapChain.h>
#include <MacOSNativeWindow.h>

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
    if (swapChain_ == nullptr || scenePipeline_ == nullptr || context_ == nullptr) return false;
    auto* swapChain = static_cast<::Diligent::ISwapChain*>(swapChain_);
    auto* context = static_cast<::Diligent::IDeviceContext*>(context_);
    auto* target = swapChain->GetCurrentBackBufferRTV();
    if (target == nullptr) return false;
    context->SetRenderTargets(1, &target, nullptr, ::Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    context->SetPipelineState(static_cast<::Diligent::IPipelineState*>(scenePipeline_));
    ::Diligent::DrawAttribs draw{};
    draw.NumVertices = 3;
    draw.Flags = ::Diligent::DRAW_FLAG_VERIFY_ALL;
    context->Draw(draw);
    swapChain->Present(syncInterval);
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

void DiligentVulkanAdapter::Shutdown() noexcept {
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
}

const std::string& DiligentVulkanAdapter::AdapterName() const noexcept {
    return adapterName_;
}

const GpuCapabilities& DiligentVulkanAdapter::Capabilities() const noexcept {
    return capabilities_;
}

} // namespace ParticleSaturn::Gpu::Diligent
