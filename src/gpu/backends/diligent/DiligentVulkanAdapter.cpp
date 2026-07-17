#include "DiligentVulkanAdapter.h"

#include "services/vulkan/VulkanDriverRuntime.h"

#include <EngineFactoryVk.h>
#include <RenderDevice.h>
#include <DeviceContext.h>
#include <SwapChain.h>
#include <MacOSNativeWindow.h>

namespace ParticleSaturn::Gpu::Diligent {
namespace {

bool IsEnabled(::Diligent::DEVICE_FEATURE_STATE state) {
    return state != ::Diligent::DEVICE_FEATURE_STATE_DISABLED;
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

void DiligentVulkanAdapter::Shutdown() noexcept {
    if (context_ != nullptr) {
        static_cast<::Diligent::IDeviceContext*>(context_)->Flush();
    }
    if (swapChain_ != nullptr) {
        static_cast<::Diligent::ISwapChain*>(swapChain_)->Release();
        swapChain_ = nullptr;
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
