#include "DiligentBackend.h"
#include "DiligentBackendInternal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <random>
#include <thread>
#include <vector>

#include "../DebugLog.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "../Settings.h"
#include "../ShaderCompileProgress.h"
#include "../generated/LogControlIcons.h"
#include "ShaderBytecodes.h"  // 构建期生成于 ${CMAKE_BINARY_DIR}/generated，经 include 目录解析

#include "ArchiverFactoryLoader.h"
#include "CommandQueueD3D12.h"
#include "CrashAnalyzer.h"
#include "DataBlobImpl.hpp"
#include "DeviceContextD3D12.h"
#include "DiligentShaderSources.h"
#include "EngineFactoryD3D11.h"
#include "EngineFactoryD3D12.h"
#include "EngineFactoryVk.h"
#include "GraphicsTypes.h"
#include "HandTracker.h"
#include "ImGuiDiligent.h"
#include "InputLayout.h"
#include "NativeWindow.h"
#include "RenderDeviceD3D11.h"
#include "RenderDeviceD3D12.h"
#include "Sampler.h"
#include "TextureViewD3D11.h" // For ITextureViewD3D11 in native D3D11 blit
#include "VulkanD3D12Interop.h"
#include "platform/windows/Win32WindowManager.h"
#include "imgui.h"
#include "md3/MD3.h"
#include "md3/MD3Log.h"  // D-015 Phase B：后端日志改写入 MD3::DebugLog，供共享面板 Log 区展示

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>            // D3DCompile for native D3D11 blit shaders
#pragma comment(lib, "d3dcompiler") // Link with d3dcompiler.lib for D3DCompile
#include <dwmapi.h>
#include <wrl/client.h> // For Microsoft::WRL::ComPtr

// D-015 后续（cc8e4a）：呈现/透明模式（DComp/D3D11 原生 blit/Backdrop/SwapChain 切换），从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::SetBackdropMode(int mode) {
    if (appState_ == nullptr || hwnd_ == nullptr) {
        return false;
    }

    // mode: 0=Solid, 1=Aero, 2=Acrylic, 3=Mica
    const bool wantTransparent = (mode != 0);

    const bool canUseDComp =
        (appState_->backdrop.transparentSupported) && (backend_ == Backend::D3D12 || backend_ == Backend::D3D11);

    // 与 Debug UI 的逻辑保持一致：透明开启时抑制 bloom，关闭时恢复。
    // 注意：这里的“透明”指的是输出 alpha 参与 DWM 合成（Backdrop 开启），与是否实际销毁/重建 SwapChain 解耦。
    const bool wasTransparent = (appState_->backdrop.useTransparent);
    if (wasTransparent != wantTransparent) {
        if (wantTransparent) {
            bloomStrengthBeforeTransp_          = appState_->render.bloomBlurStrength;
            appState_->render.bloomBlurStrength = 0.0f;
        } else {
            appState_->render.bloomBlurStrength = bloomStrengthBeforeTransp_;
        }
    }

    if (canUseDComp) {
        // 只在“需要透明但当前没有 DComp”时切换到 DComp。
        // 一旦进入 DComp 模式，不再切回普通 HWND SwapChain：
        // 在 Win11 的部分环境下，运行期反复销毁/重建 DComp SwapChain 会导致系统
        // Backdrop（Mica/Acrylic）后续再开启失效， 表现为：DWM 退化为纯色背景（用户侧观感：模糊彻底没了）。
        if (wantTransparent && !useDCompSwapChain_) {
            if (!SwitchTransparentMode(true)) {
                return false;
            }
        }
    }

    // 无论是否切换 SwapChain，都同步 DWM Backdrop（并更新 appState_->backdrop.useTransparent）
    std::cout << "[DiligentBackend] SetBackdropMode: backend="
              << (backend_ == Backend::D3D11    ? "D3D11"
                  : backend_ == Backend::D3D12  ? "D3D12"
                  : backend_ == Backend::Vulkan ? "Vulkan"
                                                : "Unknown")
              << ", mode=" << mode << ", wantTransparent=" << (wantTransparent ? "true" : "false")
              << ", useDCompSwapChain_=" << (useDCompSwapChain_ ? "true" : "false") << std::endl;
    ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd_, mode, *appState_);
    return true;
}

bool DiligentBackend::HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam) {
    if (imgui_) {
        return imgui_->HandleWin32Message(hwnd, msg, wParam, lParam);
    }
    return false;
}

ITextureView* DiligentBackend::GetCurrentBackBufferRTV() {
    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        const Backend dcompBackend = dcompSwapChain_.GetBackendType();

        if (dcompBackend == Backend::D3D11) {
            // D3D11 模式：每帧动态重建当前后缓冲的 RTV
            // 因为 D3D11 + FLIP_SEQUENTIAL 不支持同时持有多个后缓冲的 RTV
            if (!UpdateD3D11CurrentBackBufferRTV()) {
                return nullptr;
            }
            return dcompBackBufferRTVs_[0].RawPtr();
        }

        // D3D12 模式：使用预创建的 RTV
        const uint32_t idx = dcompSwapChain_.GetCurrentBackBufferIndex();
        if (idx < kDCompBufferCount && dcompBackBufferRTVs_[idx]) {
            return dcompBackBufferRTVs_[idx].RawPtr();
        }
        return nullptr;
    }
    if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        return vkD3D12Interop_->GetSharedRTV();
    }
    if (swapChain_) {
        return swapChain_->GetCurrentBackBufferRTV();
    }
    return nullptr;
}

bool DiligentBackend::CreateDCompBackBufferRTVs() {
    OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs() called\n");
    if (!dcompSwapChain_.IsInitialized() || !device_) {
        OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs: dcompSwapChain_ or device_ not ready\n");
        return false;
    }

    const uint32_t bufferCount  = dcompSwapChain_.GetBufferCount();
    const Backend  dcompBackend = dcompSwapChain_.GetBackendType();

    char dbgBuf[128];
    sprintf_s(dbgBuf, "[DiligentBackend] CreateDCompBackBufferRTVs: bufferCount=%u, backend=%d\n", bufferCount,
              static_cast<int>(dcompBackend));
    OutputDebugStringA(dbgBuf);

    if (dcompBackend == Backend::D3D12) {
        // D3D12 路径
        RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
        device_->QueryInterface(IID_RenderDeviceD3D12,
                                reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
        if (!deviceD3D12) {
            std::cerr << "[DiligentBackend] Failed to query IRenderDeviceD3D12" << std::endl;
            return false;
        }

        for (uint32_t i = 0; i < bufferCount && i < kDCompBufferCount; ++i) {
            ID3D12Resource* d3d12Resource = dcompSwapChain_.GetBackBufferD3D12(i);
            if (!d3d12Resource) {
                std::cerr << "[DiligentBackend] GetBackBufferD3D12(" << i << ") returned null" << std::endl;
                return false;
            }

            // 从 D3D12 资源创建 Diligent 纹理
            dcompBackBuffers_[i].Release();
            deviceD3D12->CreateTextureFromD3DResource(d3d12Resource, RESOURCE_STATE_PRESENT, &dcompBackBuffers_[i]);
            if (!dcompBackBuffers_[i]) {
                std::cerr << "[DiligentBackend] CreateTextureFromD3DResource failed for buffer " << i << std::endl;
                return false;
            }

            // 创建 RTV
            TextureViewDesc rtvDesc{};
            rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
            rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM_SRGB; // sRGB 视图
            dcompBackBufferRTVs_[i].Release();
            dcompBackBuffers_[i]->CreateView(rtvDesc, &dcompBackBufferRTVs_[i]);
            if (!dcompBackBufferRTVs_[i]) {
                std::cerr << "[DiligentBackend] CreateView RTV failed for buffer " << i << std::endl;
                return false;
            }
        }
    } else if (dcompBackend == Backend::D3D11) {
        // D3D11 路径
        // 注意：D3D11 + FLIP_SEQUENTIAL 模式下，不能同时为所有后缓冲创建 RTV
        // 只创建当前后缓冲的 RTV，每帧动态更新
        OutputDebugStringA("[DiligentBackend] CreateDCompBackBufferRTVs: D3D11 path\n");
        RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
        device_->QueryInterface(IID_RenderDeviceD3D11,
                                reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
        if (!deviceD3D11) {
            OutputDebugStringA("[DiligentBackend] Failed to query IRenderDeviceD3D11\n");
            std::cerr << "[DiligentBackend] Failed to query IRenderDeviceD3D11" << std::endl;
            return false;
        }
        OutputDebugStringA("[DiligentBackend] IRenderDeviceD3D11 query OK\n");

        // D3D11 只创建当前后缓冲的纹理和 RTV
        const uint32_t currentIdx = dcompSwapChain_.GetCurrentBackBufferIndex();
        char           dbgBuf[128];
        sprintf_s(dbgBuf, "[DiligentBackend] D3D11: Creating RTV for current buffer %u only\n", currentIdx);
        OutputDebugStringA(dbgBuf);

        ID3D11Texture2D* d3d11Texture = dcompSwapChain_.GetBackBufferD3D11(currentIdx);
        if (!d3d11Texture) {
            sprintf_s(dbgBuf, "[DiligentBackend] GetBackBufferD3D11(%u) returned null\n", currentIdx);
            OutputDebugStringA(dbgBuf);
            std::cerr << "[DiligentBackend] GetBackBufferD3D11(" << currentIdx << ") returned null" << std::endl;
            return false;
        }

        // 从 D3D11 资源创建 Diligent 纹理
        dcompBackBuffers_[0].Release();
        deviceD3D11->CreateTexture2DFromD3DResource(d3d11Texture, RESOURCE_STATE_RENDER_TARGET, &dcompBackBuffers_[0]);
        if (!dcompBackBuffers_[0]) {
            OutputDebugStringA("[DiligentBackend] CreateTexture2DFromD3DResource failed\n");
            std::cerr << "[DiligentBackend] CreateTexture2DFromD3DResource (D3D11) failed" << std::endl;
            return false;
        }

        // 创建 RTV
        // 注意：D3D11 不支持从 UNORM 纹理创建 SRGB 视图（D3D12 可以）
        // 所以 D3D11 必须使用 UNORM 格式，sRGB 校正需要在 shader 中手动处理
        TextureViewDesc rtvDesc{};
        rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
        rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM;
        dcompBackBufferRTVs_[0].Release();
        dcompBackBuffers_[0]->CreateView(rtvDesc, &dcompBackBufferRTVs_[0]);
        if (!dcompBackBufferRTVs_[0]) {
            OutputDebugStringA("[DiligentBackend] CreateView RTV (D3D11) failed\n");
            std::cerr << "[DiligentBackend] CreateView RTV (D3D11) failed" << std::endl;
            return false;
        }
        OutputDebugStringA("[DiligentBackend] D3D11 initial RTV created OK\n");
    } else {
        std::cerr << "[DiligentBackend] Unsupported DComp backend type" << std::endl;
        return false;
    }

    std::cout << "[DiligentBackend] Created " << bufferCount << " DComp back buffer RTVs ("
              << (dcompBackend == Backend::D3D11 ? "D3D11" : "D3D12") << ")" << std::endl;
    return true;
}

bool DiligentBackend::UpdateD3D11CurrentBackBufferRTV() {
    // D3D11 + FLIP 模式下，每帧需要重新调用 GetBuffer(0) 获取当前后缓冲
    // 不能使用缓存的后缓冲引用
    if (!dcompSwapChain_.IsInitialized() || !device_) {
        return false;
    }

    RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
    device_->QueryInterface(IID_RenderDeviceD3D11,
                            reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
    if (!deviceD3D11) {
        return false;
    }

    // D3D11 FLIP 模式：必须每帧调用 GetBuffer(0) 获取当前后缓冲
    IDXGISwapChain3* swapChain = dcompSwapChain_.GetSwapChain();
    if (!swapChain) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11Texture;
    HRESULT                                 hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&d3d11Texture));
    if (FAILED(hr) || !d3d11Texture) {
        return false;
    }

    // ============================================================================
    // 优化：检测后缓冲资源是否变化，如果相同则跳过 Diligent 纹理包装重建
    // 在 VSync 开启或帧率较低时，DXGI 可能连续帧返回相同的后缓冲
    // ============================================================================
    void* currentPtr = static_cast<void*>(d3d11Texture.Get());
    if (currentPtr == d3d11LastBackBufferPtr_ && dcompBackBufferRTVs_[0]) {
        // 资源未变化，复用现有 RTV
        return true;
    }

    // 资源变化了，必须重建
    d3d11LastBackBufferPtr_ = currentPtr;

    // 释放旧资源并重新创建
    dcompBackBuffers_[0].Release();
    dcompBackBufferRTVs_[0].Release();

    deviceD3D11->CreateTexture2DFromD3DResource(d3d11Texture.Get(), RESOURCE_STATE_RENDER_TARGET,
                                                &dcompBackBuffers_[0]);
    if (!dcompBackBuffers_[0]) {
        return false;
    }

    TextureViewDesc rtvDesc{};
    rtvDesc.ViewType = TEXTURE_VIEW_RENDER_TARGET;
    rtvDesc.Format   = TEX_FORMAT_RGBA8_UNORM; // D3D11 不支持从 UNORM 创建 SRGB 视图
    dcompBackBuffers_[0]->CreateView(rtvDesc, &dcompBackBufferRTVs_[0]);

    return dcompBackBufferRTVs_[0] != nullptr;
}

bool DiligentBackend::InitD3D11NativeBlit() {
    if (d3d11NativeBlitInitialized_) {
        return true;
    }

    if (backend_ != Backend::D3D11 || !device_) {
        return false;
    }

    // 从 Diligent 设备获取原生 D3D11 设备
    RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
    device_->QueryInterface(IID_RenderDeviceD3D11,
                            reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
    if (!deviceD3D11) {
        OutputDebugStringA("[DiligentBackend] InitD3D11NativeBlit: Failed to get D3D11 device\n");
        return false;
    }

    d3d11Device_ = deviceD3D11->GetD3D11Device();
    if (!d3d11Device_) {
        OutputDebugStringA("[DiligentBackend] InitD3D11NativeBlit: Failed to get native D3D11 device\n");
        return false;
    }

    d3d11Device_->GetImmediateContext(&d3d11Context_);
    if (!d3d11Context_) {
        OutputDebugStringA("[DiligentBackend] InitD3D11NativeBlit: Failed to get D3D11 context\n");
        return false;
    }

    // ============================================================================
    // 编译全屏 blit 着色器（内联 HLSL）
    // ============================================================================
    static const char* kBlitVS = R"(
        void main(uint vid : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
            uv.x = (vid == 1 || vid == 3) ? 2.0 : 0.0;
            uv.y = (vid == 2 || vid == 3) ? 2.0 : 0.0;
            pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
        }
    )";

    static const char* kBlitPS = R"(
        cbuffer BloomCB : register(b0) {
            float bloomStrength;
            float isTransparent;
            float isD3D11;
            float pad;
        };
        Texture2D g_Texture : register(t0);
        Texture2D g_BloomTexture : register(t1);
        SamplerState g_Sampler : register(s0);

        // 精确的线性到 sRGB 转换（IEC 61966-2-1）
        float3 LinearToSRGB(float3 color) {
            float3 srgbLow = color * 12.92;
            float3 srgbHigh = (pow(abs(color), 1.0/2.4) * 1.055) - 0.055;
            float3 srgb = (color <= 0.0031308) ? srgbLow : srgbHigh;
            return srgb;
        }

        float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
            float3 scene = g_Texture.Sample(g_Sampler, uv).rgb;
            float3 bloom = g_BloomTexture.Sample(g_Sampler, uv).rgb;
            float3 color = scene + bloom * bloomStrength;

            // 复刻标准着色器的 tone mapping（只对高光部分做压缩，强度 0.5）
            float maxRGB = max(color.r, max(color.g, color.b));
            float w = (maxRGB >= 1.0) ? 0.5 : 0.0;
            float3 toneMapped = color / (color + float3(1.0, 1.0, 1.0));
            color = lerp(color, toneMapped, w);

            // Alpha 计算（与标准着色器一致）
            float alpha = lerp(1.0, maxRGB, isTransparent);

            // D3D11 需要手动 gamma 校正（SwapChain 不支持 sRGB 格式）
            if (isD3D11 > 0.5) {
                color = LinearToSRGB(color);
            }

            // DirectComposition/DWM 要求预乘 alpha（premultiplied alpha）
            return float4(color * alpha, alpha);
        }
    )";

    // 编译顶点着色器
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    HRESULT hr = D3DCompile(kBlitVS, strlen(kBlitVS), "BlitVS", nullptr, nullptr, "main", "vs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[DiligentBackend] D3D11 VS compile error: ");
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = d3d11Device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &d3d11BlitVS_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 vertex shader\n");
        return false;
    }

    // 编译像素着色器
    hr = D3DCompile(kBlitPS, strlen(kBlitPS), "BlitPS", nullptr, nullptr, "main", "ps_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            OutputDebugStringA("[DiligentBackend] D3D11 PS compile error: ");
            OutputDebugStringA(static_cast<const char*>(errorBlob->GetBufferPointer()));
        }
        return false;
    }

    hr = d3d11Device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &d3d11BlitPS_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 pixel shader\n");
        return false;
    }

    // ============================================================================
    // 创建采样器
    // ============================================================================
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy  = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d3d11Device_->CreateSamplerState(&samplerDesc, &d3d11LinearSampler_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 sampler\n");
        return false;
    }

    // ============================================================================
    // 创建混合状态（预乘 alpha 混合）
    // ============================================================================
    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = d3d11Device_->CreateBlendState(&blendDesc, &d3d11BlendState_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 blend state\n");
        return false;
    }

    // ============================================================================
    // 创建光栅化状态
    // ============================================================================
    D3D11_RASTERIZER_DESC rasterizerDesc{};
    rasterizerDesc.FillMode              = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode              = D3D11_CULL_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthClipEnable       = TRUE;

    hr = d3d11Device_->CreateRasterizerState(&rasterizerDesc, &d3d11RasterizerState_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 rasterizer state\n");
        return false;
    }

    // ============================================================================
    // 创建深度模板状态（禁用深度测试）
    // ============================================================================
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
    depthStencilDesc.DepthEnable    = FALSE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.StencilEnable  = FALSE;

    hr = d3d11Device_->CreateDepthStencilState(&depthStencilDesc, &d3d11DepthStencilState_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 depth stencil state\n");
        return false;
    }

    // ============================================================================
    // 创建常量缓冲
    // ============================================================================
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth      = 16; // 4 floats
    cbDesc.Usage          = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = d3d11Device_->CreateBuffer(&cbDesc, nullptr, &d3d11BloomCB_);
    if (FAILED(hr)) {
        OutputDebugStringA("[DiligentBackend] Failed to create D3D11 constant buffer\n");
        return false;
    }

    d3d11NativeBlitInitialized_ = true;
    OutputDebugStringA("[DiligentBackend] D3D11 native blit pipeline initialized\n");
    return true;
}

void DiligentBackend::BlitOffscreenToBackBufferD3D11() {
    if (!d3d11NativeBlitInitialized_ || !d3d11Context_ || !d3d11Device_) {
        // 回退到标准路径
        BlitOffscreenToBackBuffer();
        return;
    }

    // 获取后缓冲并创建/更新 RTV
    IDXGISwapChain3* swapChain = dcompSwapChain_.GetSwapChain();
    if (!swapChain) {
        BlitOffscreenToBackBuffer();
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT                                 hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        BlitOffscreenToBackBuffer();
        return;
    }

    // 检测后缓冲是否变化，如果变化则重建 RTV
    // 注意：不能只靠指针比较，因为 ResizeBuffers 后新后缓冲可能复用旧地址
    // 需要检查尺寸是否匹配
    bool needRecreateRTV = !d3d11CachedRTV_;
    if (!needRecreateRTV && d3d11CachedRTV_) {
        // 检查 RTV 对应的纹理尺寸是否与当前 SwapChain 尺寸匹配
        D3D11_TEXTURE2D_DESC backBufferDesc{};
        backBuffer->GetDesc(&backBufferDesc);
        if (backBufferDesc.Width != dcompSwapChain_.GetWidth() ||
            backBufferDesc.Height != dcompSwapChain_.GetHeight()) {
            needRecreateRTV = true;
        }
    }
    if (needRecreateRTV) {
        d3d11LastBackBufferPtr_ = static_cast<void*>(backBuffer.Get());
        d3d11CachedRTV_.Reset();
        hr = d3d11Device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &d3d11CachedRTV_);
        if (FAILED(hr)) {
            BlitOffscreenToBackBuffer();
            return;
        }
    }

    // 获取源纹理的原生 SRV
    // 优化：当 UI Blur 启用时，复用 uiSceneSRV_
    const bool    useUISceneAsSource = (appState_ != nullptr && appState_->ui.blurEnabled && uiSceneSRV_ != nullptr);
    ITextureView* srcSRV             = useUISceneAsSource ? uiSceneSRV_.RawPtr() : offscreenSRV_.RawPtr();
    ITextureView* bloomSRV           = bloomSRV_B_ ? bloomSRV_B_.RawPtr() : offscreenSRV_.RawPtr();

    if (!srcSRV || !bloomSRV) {
        BlitOffscreenToBackBuffer();
        return;
    }

    // 从 Diligent ITextureView 获取原生 D3D11 SRV
    RefCntAutoPtr<ITextureViewD3D11> srcViewD3D11, bloomViewD3D11;
    srcSRV->QueryInterface(IID_TextureViewD3D11,
                           reinterpret_cast<IObject**>(static_cast<ITextureViewD3D11**>(&srcViewD3D11)));
    bloomSRV->QueryInterface(IID_TextureViewD3D11,
                             reinterpret_cast<IObject**>(static_cast<ITextureViewD3D11**>(&bloomViewD3D11)));

    if (!srcViewD3D11 || !bloomViewD3D11) {
        BlitOffscreenToBackBuffer();
        return;
    }

    ID3D11ShaderResourceView* srcD3D11SRV   = static_cast<ID3D11ShaderResourceView*>(srcViewD3D11->GetD3D11View());
    ID3D11ShaderResourceView* bloomD3D11SRV = static_cast<ID3D11ShaderResourceView*>(bloomViewD3D11->GetD3D11View());

    if (!srcD3D11SRV || !bloomD3D11SRV) {
        BlitOffscreenToBackBuffer();
        return;
    }

    // ============================================================================
    // 使用原生 D3D11 API 进行渲染
    // ============================================================================

    // 更新常量缓冲
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = d3d11Context_->Map(d3d11BloomCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        struct BloomCB {
            float strength;
            float transparent;
            float isD3D11;
            float pad;
        };

        auto* cb        = static_cast<BloomCB*>(mapped.pData);
        cb->strength    = useUISceneAsSource ? 0.0f
            : ((appState_ != nullptr && appState_->render.bloomEnabled)
                   ? std::max(0.0f, appState_->render.bloomBlurStrength)
                   : 0.0f);
        cb->transparent = (appState_ != nullptr && appState_->backdrop.useTransparent) ? 1.0f : 0.0f;
        cb->isD3D11     = 1.0f;
        cb->pad         = 0.0f;
        d3d11Context_->Unmap(d3d11BloomCB_.Get(), 0);
    }

    // 设置渲染目标
    ID3D11RenderTargetView* rtvs[] = {d3d11CachedRTV_.Get()};
    d3d11Context_->OMSetRenderTargets(1, rtvs, nullptr);

    // 设置视口 - 使用 SwapChain 的实际尺寸，确保 resize 后正确
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(dcompSwapChain_.GetWidth());
    vp.Height   = static_cast<float>(dcompSwapChain_.GetHeight());
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    d3d11Context_->RSSetViewports(1, &vp);

    // 设置管线状态
    d3d11Context_->VSSetShader(d3d11BlitVS_.Get(), nullptr, 0);
    d3d11Context_->PSSetShader(d3d11BlitPS_.Get(), nullptr, 0);
    d3d11Context_->RSSetState(d3d11RasterizerState_.Get());
    d3d11Context_->OMSetDepthStencilState(d3d11DepthStencilState_.Get(), 0);

    float blendFactor[4] = {0, 0, 0, 0};
    d3d11Context_->OMSetBlendState(d3d11BlendState_.Get(), blendFactor, 0xFFFFFFFF);

    // 绑定资源
    ID3D11Buffer* cbs[] = {d3d11BloomCB_.Get()};
    d3d11Context_->PSSetConstantBuffers(0, 1, cbs);

    ID3D11ShaderResourceView* srvs[] = {srcD3D11SRV, bloomD3D11SRV};
    d3d11Context_->PSSetShaderResources(0, 2, srvs);

    ID3D11SamplerState* samplers[] = {d3d11LinearSampler_.Get()};
    d3d11Context_->PSSetSamplers(0, 1, samplers);

    // 设置图元拓扑和输入布局
    d3d11Context_->IASetInputLayout(nullptr);
    d3d11Context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // 绘制全屏四边形
    d3d11Context_->Draw(4, 0);

    // 清除绑定，避免资源冲突
    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
    d3d11Context_->PSSetShaderResources(0, 2, nullSRVs);
    ID3D11RenderTargetView* nullRTVs[1] = {nullptr};
    d3d11Context_->OMSetRenderTargets(1, nullRTVs, nullptr);
}

void DiligentBackend::PresentFrame(int syncInterval) {
    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        // DirectComposition 模式：需要手动 Flush + FinishFrame 来确保引擎正确回收资源
        immediateContext_->Flush();
        dcompSwapChain_.Present(static_cast<uint32_t>(syncInterval));
        // 通知引擎帧结束，回收动态描述符和过期资源
        immediateContext_->FinishFrame();
    } else if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        // Vulkan 互操作模式
        vkD3D12Interop_->FlushVulkan(); // 确保 Vulkan 渲染完成
        vkD3D12Interop_->Present(static_cast<uint32_t>(syncInterval));
        immediateContext_->FinishFrame();
    } else if (swapChain_) {
        // 标准模式：SwapChain->Present() 内部会调用 FinishFrame()
        swapChain_->Present(static_cast<Uint32>(syncInterval));
    }
}

bool DiligentBackend::SwitchTransparentMode(bool enableTransparent) {
    // 支持 D3D11 和 D3D12，Vulkan 暂不支持透明模式
    if (backend_ != Backend::D3D12 && backend_ != Backend::D3D11) {
        std::cerr << "[DiligentBackend] SwitchTransparentMode: backend not supported (D3D11/D3D12 only)" << std::endl;
        return false;
    }

    // 检查当前状态
    bool currentlyTransparent = false;
    if (backend_ == Backend::D3D12 || backend_ == Backend::D3D11) {
        currentlyTransparent = useDCompSwapChain_;
    } else if (backend_ == Backend::Vulkan) {
        currentlyTransparent = useVkD3D12Interop_;
    }

    if (currentlyTransparent == enableTransparent) {
        return true; // 无需切换
    }

    std::cout << "[DiligentBackend] Switching transparent mode: " << (enableTransparent ? "ON" : "OFF") << std::endl;

    // 1. 用深色/浅色清空当前帧，减少闪烁刺激
    const bool  isDarkMode = (appState_ != nullptr) ? appState_->ui.darkMode : true;
    const float clearR     = isDarkMode ? 0.0f : 1.0f;
    const float clearG     = isDarkMode ? 0.0f : 1.0f;
    const float clearB     = isDarkMode ? 0.0f : 1.0f;
    const float clearA     = 1.0f;

    {
        ITextureView* rtv = GetCurrentBackBufferRTV();
        if (rtv) {
            const float clearColor[] = {clearR, clearG, clearB, clearA};
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->ClearRenderTarget(rtv, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        PresentFrame(0); // 立即呈现，不等 VSync
    }

    // 2. 等待 GPU 完成所有工作
    if (backend_ == Backend::Vulkan && useVkD3D12Interop_ && vkD3D12Interop_) {
        vkD3D12Interop_->FlushVulkan();
    }
    immediateContext_->Flush();
    immediateContext_->FinishFrame();
    immediateContext_->WaitForIdle();

    // 3. 释放依赖 SwapChain 格式的 PSO 和绑定
    fullscreenQuadPSO_.Release();
    fullscreenQuadSRB_.Release();

    // 3.5 强制刷新窗口，清除 DWM 可能缓存的旧帧内容
    // 这对于从非透明切换到透明模式尤为重要
    if (enableTransparent && hwnd_) {
        // 使窗口无效并强制重绘
        InvalidateRect(hwnd_, nullptr, TRUE);
        UpdateWindow(hwnd_);
        // 给 DWM 一点时间刷新
        Sleep(10);
    }

    // 4. 销毁当前 SwapChain 资源
    if (backend_ == Backend::D3D12 || backend_ == Backend::D3D11) {
        if (useDCompSwapChain_) {
            // 释放 DComp 后缓冲
            for (auto& rtv : dcompBackBufferRTVs_) {
                rtv.Release();
            }
            for (auto& buf : dcompBackBuffers_) {
                buf.Release();
            }
            dcompSwapChain_.Shutdown();
            useDCompSwapChain_ = false;
        } else {
            swapChain_.Release();
        }
    } else if (backend_ == Backend::Vulkan) {
        if (useVkD3D12Interop_) {
            vkD3D12Interop_.reset();
            useVkD3D12Interop_ = false;
        } else {
            swapChain_.Release();
        }
    }

    // 4.5 窗口扩展样式（WS_EX_NOREDIRECTIONBITMAP）处理：
    // 该标志在本项目里用于 DirectComposition 透明 SwapChain。
    //
    // 注意：实际运行中把该标志“关掉再打开”会导致后续再开启系统 Backdrop（Mica/Acrylic）失效，
    // 现象为：DWM 仍显示纯色背景（用户侧观感：模糊彻底没了）。
    //
    // 因此这里改为“只确保开启，不主动关闭”，让窗口在支持平台上始终保持该标志，
    // 从而保证透明/Backdrop 可以稳定在运行期反复切换。
    if (enableTransparent && hwnd_) {
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        const LONG_PTR desired = exStyle | static_cast<LONG_PTR>(WS_EX_NOREDIRECTIONBITMAP);
        if (desired != exStyle) {
            SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, desired);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    // 5. 创建新的 SwapChain / Interop
    if (enableTransparent) {
        // 重要：在创建 DirectComposition 之前先设置 DWM backdrop（与启动时顺序一致）
        if (appState_ != nullptr && hwnd_ != nullptr) {
            const int micaMode = 3; // Mica
            ParticleSaturn::Win32WindowManager::SetBackdropMode(hwnd_, micaMode, *appState_);
            DwmFlush();
        }
        if (backend_ == Backend::D3D12) {
            // 切换到 DirectComposition 模式
            RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
            device_->QueryInterface(IID_RenderDeviceD3D12,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
            if (!deviceD3D12) {
                std::cerr << "[DiligentBackend] Failed to get IRenderDeviceD3D12" << std::endl;
                return false;
            }

            ID3D12Device* d3d12Device = deviceD3D12->GetD3D12Device();

            // 获取命令队列
            ICommandQueue* cmdQueueBase = immediateContext_->LockCommandQueue();
            if (!cmdQueueBase) {
                std::cerr << "[DiligentBackend] Failed to lock command queue" << std::endl;
                return false;
            }

            RefCntAutoPtr<ICommandQueueD3D12> cmdQueueD3D12;
            cmdQueueBase->QueryInterface(
                IID_CommandQueueD3D12, reinterpret_cast<IObject**>(static_cast<ICommandQueueD3D12**>(&cmdQueueD3D12)));
            if (!cmdQueueD3D12) {
                immediateContext_->UnlockCommandQueue();
                std::cerr << "[DiligentBackend] Failed to get ICommandQueueD3D12" << std::endl;
                return false;
            }

            ID3D12CommandQueue* cmdQueue = cmdQueueD3D12->GetD3D12CommandQueue();

            if (!dcompSwapChain_.InitD3D12(hwnd_, d3d12Device, cmdQueue, surfaceSize_.Width, surfaceSize_.Height,
                                           kDCompBufferCount)) {
                immediateContext_->UnlockCommandQueue();
                std::cerr << "[DiligentBackend] Failed to init DComp SwapChain" << std::endl;
                return false;
            }

            immediateContext_->UnlockCommandQueue();

            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to create DComp back buffer RTVs" << std::endl;
                return false;
            }

            useDCompSwapChain_ = true;
        } else if (backend_ == Backend::D3D11) {
            // D3D11 切换到 DirectComposition 模式
            RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
            device_->QueryInterface(IID_RenderDeviceD3D11,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
            if (!deviceD3D11) {
                std::cerr << "[DiligentBackend] Failed to get IRenderDeviceD3D11" << std::endl;
                return false;
            }

            ID3D11Device* d3d11Device = deviceD3D11->GetD3D11Device();

            if (!dcompSwapChain_.InitD3D11(hwnd_, d3d11Device, surfaceSize_.Width, surfaceSize_.Height,
                                           kDCompBufferCount)) {
                std::cerr << "[DiligentBackend] Failed to init D3D11 DComp SwapChain" << std::endl;
                return false;
            }

            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to create D3D11 DComp back buffer RTVs" << std::endl;
                return false;
            }

            useDCompSwapChain_ = true;
        } else if (backend_ == Backend::Vulkan) {
            // Vulkan 透明模式
            vkD3D12Interop_ = std::make_unique<VulkanD3D12Interop>();
            if (!vkD3D12Interop_->Init(hwnd_, device_, immediateContext_, surfaceSize_.Width, surfaceSize_.Height)) {
                std::cerr << "[DiligentBackend] Failed to init VulkanD3D12Interop" << std::endl;
                vkD3D12Interop_.reset();
                return false;
            }
            useVkD3D12Interop_ = true;
            // 同步 DWM
            DwmFlush();
        }
    } else {
        // 切换到标准 SwapChain 模式
        SwapChainDesc scDesc{};
        scDesc.Width             = surfaceSize_.Width;
        scDesc.Height            = surfaceSize_.Height;
        scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
        scDesc.BufferCount       = 3;
        scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;

        const NativeWindow window{reinterpret_cast<void*>(hwnd_)};

        if (backend_ == Backend::D3D12) {
            auto* factory = GetEngineFactoryD3D12();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get D3D12 factory" << std::endl;
                return false;
            }
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
            useDCompSwapChain_ = false;
        } else if (backend_ == Backend::D3D11) {
            auto* factory = GetEngineFactoryD3D11();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get D3D11 factory" << std::endl;
                return false;
            }
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D11(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);
            useDCompSwapChain_ = false;
        } else if (backend_ == Backend::Vulkan) {
            auto* factory = GetEngineFactoryVk();
            if (!factory) {
                std::cerr << "[DiligentBackend] Failed to get Vulkan factory" << std::endl;
                return false;
            }
            factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);
            useVkD3D12Interop_ = false;
        }

        if (!swapChain_) {
            std::cerr << "[DiligentBackend] Failed to create standard SwapChain" << std::endl;
            return false;
        }
    }

    // 6. 重建依赖 SwapChain 格式的 PSO
    if (!CreateFullscreenQuadPSO()) {
        std::cerr << "[DiligentBackend] Failed to recreate fullscreen quad PSO" << std::endl;
        return false;
    }
    UpdateFullscreenQuadBindings();

    // 7. 切换后立即清空一帧（透明模式使用 alpha=0，不透明模式使用 alpha=1）
    {
        ITextureView* rtv = GetCurrentBackBufferRTV();
        if (rtv) {
            // 透明模式下需要 alpha=0，否则 DWM 会把内容当作不透明
            const float finalAlpha   = enableTransparent ? 0.0f : 1.0f;
            const float clearColor[] = {clearR, clearG, clearB, finalAlpha};
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            immediateContext_->ClearRenderTarget(rtv, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
        PresentFrame(0);
    }

    // 更新状态
    if (appState_ != nullptr) {
        appState_->backdrop.useTransparent = enableTransparent;
    }

    std::cout << "[DiligentBackend] Transparent mode switched successfully" << std::endl;
    return true;
}

} // namespace ParticleSaturn::Render
