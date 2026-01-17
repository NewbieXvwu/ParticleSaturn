#include "DirectCompositionSwapChain.h"

#include <iostream>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

namespace ParticleSaturn::Render {

DirectCompositionSwapChain::~DirectCompositionSwapChain() {
    Shutdown();
}

bool DirectCompositionSwapChain::Init(HWND hwnd, ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12CmdQueue,
                                      uint32_t width, uint32_t height, uint32_t bufferCount) {
    if (hwnd == nullptr || d3d12Device == nullptr || d3d12CmdQueue == nullptr) {
        std::cerr << "[DCompSwapChain] Invalid parameters" << std::endl;
        return false;
    }

    hwnd_        = hwnd;
    width_       = width;
    height_      = height;
    bufferCount_ = bufferCount;

    // 1. 创建 DirectComposition 设备
    HRESULT hr = DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&dcompDevice_));
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] DCompositionCreateDevice failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[DCompSwapChain] DirectComposition device created" << std::endl;

    // 2. 创建 DirectComposition target（绑定到 HWND）
    hr = dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] CreateTargetForHwnd failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[DCompSwapChain] DirectComposition target created" << std::endl;

    // 3. 创建 DirectComposition visual
    hr = dcompDevice_->CreateVisual(&dcompVisual_);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] CreateVisual failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    std::cout << "[DCompSwapChain] DirectComposition visual created" << std::endl;

    // 4. 创建 DXGI Factory
    Microsoft::WRL::ComPtr<IDXGIFactory4> dxgiFactory;
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
    hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&dxgiFactory));
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] CreateDXGIFactory2 failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 5. 创建 SwapChain（使用 CreateSwapChainForComposition + PREMULTIPLIED alpha）
    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width              = width;
    scDesc.Height             = height;
    scDesc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.Stereo             = FALSE;
    scDesc.SampleDesc.Count   = 1;
    scDesc.SampleDesc.Quality = 0;
    scDesc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount        = bufferCount;
    scDesc.Scaling            = DXGI_SCALING_STRETCH;
    scDesc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode          = DXGI_ALPHA_MODE_PREMULTIPLIED; // 关键！
    scDesc.Flags              = 0;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = dxgiFactory->CreateSwapChainForComposition(d3d12CmdQueue, &scDesc, nullptr, &swapChain1);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] CreateSwapChainForComposition failed: 0x" << std::hex << hr << std::dec
                  << std::endl;
        return false;
    }

    hr = swapChain1.As(&swapChain_);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] QueryInterface IDXGISwapChain3 failed: 0x" << std::hex << hr << std::dec
                  << std::endl;
        return false;
    }
    std::cout << "[DCompSwapChain] SwapChain created with PREMULTIPLIED alpha mode" << std::endl;

    // 6. 获取后缓冲引用
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        if (FAILED(hr)) {
            std::cerr << "[DCompSwapChain] GetBuffer(" << i << ") failed: 0x" << std::hex << hr << std::dec
                      << std::endl;
            return false;
        }
    }

    // 7. 将 SwapChain 设置为 visual 的内容
    hr = dcompVisual_->SetContent(swapChain_.Get());
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] SetContent failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 8. 将 visual 设置为 target 的根
    hr = dcompTarget_->SetRoot(dcompVisual_.Get());
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] SetRoot failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // 9. 提交 DirectComposition
    hr = dcompDevice_->Commit();
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] Commit failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    std::cout << "[DCompSwapChain] DirectComposition initialized successfully (" << width << "x" << height << ", "
              << bufferCount << " buffers)" << std::endl;
    return true;
}

void DirectCompositionSwapChain::Shutdown() {
    for (auto& buf : backBuffers_) {
        buf.Reset();
    }
    swapChain_.Reset();
    dcompVisual_.Reset();
    dcompTarget_.Reset();
    dcompDevice_.Reset();
    hwnd_        = nullptr;
    width_       = 0;
    height_      = 0;
    bufferCount_ = 0;
}

bool DirectCompositionSwapChain::Resize(uint32_t width, uint32_t height) {
    if (!swapChain_) {
        return false;
    }

    // 释放后缓冲引用
    for (auto& buf : backBuffers_) {
        buf.Reset();
    }

    HRESULT hr = swapChain_->ResizeBuffers(bufferCount_, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] ResizeBuffers failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    width_  = width;
    height_ = height;

    // 重新获取后缓冲引用
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        if (FAILED(hr)) {
            std::cerr << "[DCompSwapChain] GetBuffer(" << i << ") after resize failed: 0x" << std::hex << hr << std::dec
                      << std::endl;
            return false;
        }
    }

    std::cout << "[DCompSwapChain] Resized to " << width << "x" << height << std::endl;
    return true;
}

HRESULT DirectCompositionSwapChain::Present(uint32_t syncInterval) {
    if (!swapChain_) {
        return E_FAIL;
    }
    return swapChain_->Present(syncInterval, 0);
}

uint32_t DirectCompositionSwapChain::GetCurrentBackBufferIndex() const {
    if (!swapChain_) {
        return 0;
    }
    return swapChain_->GetCurrentBackBufferIndex();
}

ID3D12Resource* DirectCompositionSwapChain::GetBackBuffer(uint32_t index) const {
    if (index >= bufferCount_) {
        return nullptr;
    }
    return backBuffers_[index].Get();
}

} // namespace ParticleSaturn::Render
