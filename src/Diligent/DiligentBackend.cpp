#include "DiligentBackend.h"

#include "NativeWindow.h"

#include "EngineD3D12.h"
#include "EngineFactoryD3D12.h"

#include "EngineFactoryVk.h"
#include "EngineVk.h"

namespace ParticleSaturn::Render {

using namespace Diligent;

bool DiligentBackend::Init(Backend backend, HWND hwnd, SurfaceSize initialSize) {
    backend_ = backend;

    if (hwnd == nullptr || initialSize.Width == 0 || initialSize.Height == 0) {
        return false;
    }

    device_.Release();
    immediateContext_.Release();
    swapChain_.Release();

    const NativeWindow window{reinterpret_cast<void*>(hwnd)};

    SwapChainDesc scDesc{};
    scDesc.Width  = initialSize.Width;
    scDesc.Height = initialSize.Height;
    // 先不创建深度缓冲：当前里程碑只验证 swapchain + present 能跑。
    scDesc.DepthBufferFormat = TEX_FORMAT_UNKNOWN;

    if (backend == Backend::D3D12) {
        auto* factory = GetEngineFactoryD3D12();
        if (factory == nullptr) {
            return false;
        }

        EngineD3D12CreateInfo engineCI{};
        factory->CreateDeviceAndContextsD3D12(engineCI, &device_, &immediateContext_);

        FullScreenModeDesc fsDesc{};
        factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
    } else {
        auto* factory = GetEngineFactoryVk();
        if (factory == nullptr) {
            return false;
        }

        EngineVkCreateInfo engineCI{};
        factory->CreateDeviceAndContextsVk(engineCI, &device_, &immediateContext_);

        factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);
    }

    return swapChain_ != nullptr;
}

void DiligentBackend::Shutdown() {
    swapChain_.Release();
    immediateContext_.Release();
    device_.Release();
}

void DiligentBackend::Resize(SurfaceSize newSize) {
    if (swapChain_ == nullptr) {
        return;
    }
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }

    const auto& curDesc = swapChain_->GetDesc();
    if (curDesc.Width == newSize.Width && curDesc.Height == newSize.Height) {
        return;
    }

    swapChain_->Resize(newSize.Width, newSize.Height);
}

void DiligentBackend::RenderClear() {
    if (swapChain_ == nullptr || immediateContext_ == nullptr) {
        return;
    }

    ITextureView* pRTV = swapChain_->GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }

    immediateContext_->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 颜色随时间变化留到后续；此处用固定色验证呈现链路。
    const float clearColor[4] = {0.05f, 0.07f, 0.10f, 1.0f};
    immediateContext_->ClearRenderTarget(pRTV, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DiligentBackend::RenderFrame() {
    if (swapChain_ == nullptr) {
        return;
    }

    RenderClear();
    swapChain_->Present(1);
}

} // namespace ParticleSaturn::Render
