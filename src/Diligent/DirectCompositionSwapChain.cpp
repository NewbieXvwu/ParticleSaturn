#include "DirectCompositionSwapChain.h"

#include <iostream>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

namespace ParticleSaturn::Render {

DirectCompositionSwapChain::~DirectCompositionSwapChain() {
    Shutdown();
}

bool DirectCompositionSwapChain::InitD3D12(HWND hwnd, ID3D12Device* d3d12Device, ID3D12CommandQueue* d3d12CmdQueue,
                                           uint32_t width, uint32_t height, uint32_t bufferCount) {
    if (hwnd == nullptr || d3d12Device == nullptr || d3d12CmdQueue == nullptr) {
        std::cerr << "[DCompSwapChain] D3D12: Invalid parameters" << std::endl;
        return false;
    }
    return InitCommon(hwnd, d3d12CmdQueue, width, height, bufferCount, Backend::D3D12);
}

bool DirectCompositionSwapChain::InitD3D11(HWND hwnd, ID3D11Device* d3d11Device, uint32_t width, uint32_t height,
                                           uint32_t bufferCount) {
    if (hwnd == nullptr || d3d11Device == nullptr) {
        std::cerr << "[DCompSwapChain] D3D11: Invalid parameters" << std::endl;
        return false;
    }
    return InitCommon(hwnd, d3d11Device, width, height, bufferCount, Backend::D3D11);
}

bool DirectCompositionSwapChain::InitCommon(HWND hwnd, IUnknown* deviceOrQueue, uint32_t width, uint32_t height,
                                            uint32_t bufferCount, Backend backend) {
    if (bufferCount == 0 || bufferCount > 3) {
        std::cerr << "[DCompSwapChain] Invalid bufferCount=" << bufferCount << " (supported: 1..3)" << std::endl;
        return false;
    }

    hwnd_        = hwnd;
    width_       = width;
    height_      = height;
    bufferCount_ = bufferCount;
    backendType_ = backend;

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
    hr = dxgiFactory->CreateSwapChainForComposition(deviceOrQueue, &scDesc, nullptr, &swapChain1);
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
    std::cout << "[DCompSwapChain] SwapChain created with PREMULTIPLIED alpha mode ("
              << (backend == Backend::D3D11 ? "D3D11" : "D3D12") << ")" << std::endl;

    // 6. 获取后缓冲引用
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        if (backend == Backend::D3D12) {
            hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d12BackBuffers_[i]));
        } else {
            hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d11BackBuffers_[i]));
        }
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
    for (auto& buf : d3d12BackBuffers_) {
        buf.Reset();
    }
    for (auto& buf : d3d11BackBuffers_) {
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
    d3d11CurrentBackBufferIndex_ = 0;
}

bool DirectCompositionSwapChain::Resize(uint32_t width, uint32_t height) {
    if (!swapChain_) {
        return false;
    }

    // DXGI ResizeBuffers 要求：调用前必须释放所有旧 backbuffer 引用，否则会 DXGI_ERROR_INVALID_CALL。
    // 重要：这里不能“为了失败回滚”而先保存 ComPtr 副本 —— 那会增加引用计数，导致 ResizeBuffers 永远失败。
    // 如果 ResizeBuffers 失败，可通过 GetBuffer() 重新获取当前（旧尺寸）缓冲来恢复渲染。
    for (uint32_t i = 0; i < 3; ++i) {
        d3d12BackBuffers_[i].Reset();
        d3d11BackBuffers_[i].Reset();
    }

    // 使用 0/UNKNOWN 保持原有 bufferCount/format，降低驱动对参数一致性的敏感度。
    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        std::cerr << "[DCompSwapChain] ResizeBuffers failed: 0x" << std::hex << hr << std::dec << std::endl;
        // 恢复当前 backbuffer 引用，避免进入“无法继续渲染”的坏状态。
        for (uint32_t i = 0; i < bufferCount_; ++i) {
            HRESULT hrBuf = S_OK;
            if (backendType_ == Backend::D3D12) {
                hrBuf = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d12BackBuffers_[i]));
            } else {
                hrBuf = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d11BackBuffers_[i]));
            }
            if (FAILED(hrBuf)) {
                std::cerr << "[DCompSwapChain] GetBuffer(" << i << ") after resize failure failed: 0x" << std::hex
                          << hrBuf << std::dec << std::endl;
            }
        }
        return false;
    }

    width_  = width;
    height_ = height;
    d3d11CurrentBackBufferIndex_ = 0;

    // 重新获取后缓冲引用
    for (uint32_t i = 0; i < bufferCount_; ++i) {
        if (backendType_ == Backend::D3D12) {
            hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d12BackBuffers_[i]));
        } else {
            hr = swapChain_->GetBuffer(i, IID_PPV_ARGS(&d3d11BackBuffers_[i]));
        }
        if (FAILED(hr)) {
            std::cerr << "[DCompSwapChain] GetBuffer(" << i << ") after resize failed: 0x" << std::hex << hr << std::dec
                      << std::endl;
            return false;
        }
    }

    // 某些情况下（尤其是频繁 resize）需要额外 commit 以确保 DComp 及时刷新。
    // 重新设置 visual 的内容以确保 DComp 正确更新 SwapChain 的新尺寸
    if (dcompVisual_ && swapChain_) {
        hr = dcompVisual_->SetContent(swapChain_.Get());
        if (FAILED(hr)) {
            std::cerr << "[DCompSwapChain] SetContent after resize failed: 0x" << std::hex << hr << std::dec
                      << std::endl;
        }
    }
    if (dcompDevice_) {
        (void)dcompDevice_->Commit();
    }

    std::cout << "[DCompSwapChain] Resized to " << width << "x" << height << std::endl;
    return true;
}

HRESULT DirectCompositionSwapChain::Present(uint32_t syncInterval) {
    if (!swapChain_) {
        return E_FAIL;
    }
    HRESULT hr = swapChain_->Present(syncInterval, 0);

    // D3D11：手动跟踪后缓冲索引（FLIP_SEQUENTIAL 模式下轮换）
    if (backendType_ == Backend::D3D11 && SUCCEEDED(hr)) {
        d3d11CurrentBackBufferIndex_ = (d3d11CurrentBackBufferIndex_ + 1) % bufferCount_;
    }

    return hr;
}

uint32_t DirectCompositionSwapChain::GetCurrentBackBufferIndex() const {
    if (!swapChain_) {
        return 0;
    }
    // D3D12：使用 IDXGISwapChain3::GetCurrentBackBufferIndex
    // D3D11：使用手动跟踪的索引
    if (backendType_ == Backend::D3D12) {
        return swapChain_->GetCurrentBackBufferIndex();
    } else {
        return d3d11CurrentBackBufferIndex_;
    }
}

ID3D12Resource* DirectCompositionSwapChain::GetBackBufferD3D12(uint32_t index) const {
    if (index >= bufferCount_ || backendType_ != Backend::D3D12) {
        return nullptr;
    }
    return d3d12BackBuffers_[index].Get();
}

ID3D11Texture2D* DirectCompositionSwapChain::GetBackBufferD3D11(uint32_t index) const {
    if (index >= bufferCount_ || backendType_ != Backend::D3D11) {
        return nullptr;
    }
    return d3d11BackBuffers_[index].Get();
}

} // namespace ParticleSaturn::Render
