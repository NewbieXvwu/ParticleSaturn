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

// D-015 后续（cc8e4a）：UI 场景解析/模糊/Acrylic 合成与日志图标，从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateAcrylicPSO() {
    if (device_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> vs, ps;

    vs = PS_SHADER_FROM_BYTECODE(device_, backend_, "AcrylicComposite VS", SHADER_TYPE_VERTEX, AcrylicComposite_VS);
    ps = PS_SHADER_FROM_BYTECODE(device_, backend_, "AcrylicComposite PS", SHADER_TYPE_PIXEL, AcrylicComposite_PS);

    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    // 常量缓冲：2个 float4（tint + params）
    if (acrylicConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Acrylic Constants";
        cbDesc.Size           = 32;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
        device_->CreateBuffer(cbDesc, nullptr, &acrylicConstants_);
        if (acrylicConstants_ == nullptr) {
            return false;
        }
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "AcrylicComposite PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN;

    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "AcrylicCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW  = TEXTURE_ADDRESS_CLAMP;

    const char*                samplerName     = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";
    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, samplerName, sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    acrylicPSO_.Release();
    acrylicSRB_.Release();
    CreateGraphicsPSO(device_, psoCI, &acrylicPSO_);
    if (acrylicPSO_ == nullptr) {
        return false;
    }

    if (auto* var = acrylicPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "AcrylicCB"); var != nullptr) {
        var->Set(acrylicConstants_);
    }

    acrylicPSO_->CreateShaderResourceBinding(&acrylicSRB_, true);
    if (acrylicSRB_ != nullptr) {
        // 缓存热路径变量指针
        acrylicTexVar_ = acrylicSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    }
    return acrylicSRB_ != nullptr;
}

bool DiligentBackend::CreateUISceneTextures(SurfaceSize size) {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return true;
    }

    // 根据当前模式确定 RTV 格式
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM; // DirectComposition 使用非 sRGB
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }

    const uint32_t w   = size.Width;
    const uint32_t h   = size.Height;
    const uint32_t w6  = std::max(1u, size.Width / 6u);
    const uint32_t h6  = std::max(1u, size.Height / 6u);
    const uint32_t w12 = std::max(1u, size.Width / 12u);
    const uint32_t h12 = std::max(1u, size.Height / 12u);

    bool sceneSizeChanged = true;
    if (uiSceneColor_ != nullptr) {
        const auto& desc = uiSceneColor_->GetDesc();
        sceneSizeChanged = (desc.Width != w || desc.Height != h || desc.Format != rtvFormat);
    }

    const bool sizeChanged =
        sceneSizeChanged || (uiBlurW_ != w6 || uiBlurH_ != h6 || uiBlurW2_ != w12 || uiBlurH2_ != h12);
    if (!sizeChanged && uiSceneColor_ != nullptr && uiBlurTexA_ != nullptr && uiBlurTexB_ != nullptr &&
        uiBlurTexC_ != nullptr && uiBlurTexD_ != nullptr && uiAcrylicStrong_ != nullptr && uiAcrylicWeak_ != nullptr &&
        uiNoiseTex_ != nullptr) {
        return true;
    }

    uiBlurW_  = w6;
    uiBlurH_  = h6;
    uiBlurW2_ = w12;
    uiBlurH2_ = h12;

    auto createTex = [&](const char* name, TEXTURE_FORMAT fmt, uint32_t texW, uint32_t texH,
                         RefCntAutoPtr<ITexture>& outTex, RefCntAutoPtr<ITextureView>& outRTV,
                         RefCntAutoPtr<ITextureView>& outSRV) -> bool {
        TextureDesc texDesc{};
        texDesc.Name      = name;
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = texW;
        texDesc.Height    = texH;
        texDesc.MipLevels = 1;
        texDesc.Format    = fmt;
        texDesc.Usage     = USAGE_DEFAULT;
        texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;

        outTex.Release();
        outRTV.Release();
        outSRV.Release();

        device_->CreateTexture(texDesc, nullptr, &outTex);
        if (outTex == nullptr) {
            return false;
        }
        outRTV = outTex->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
        outSRV = outTex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        return outRTV != nullptr && outSRV != nullptr;
    };

    // 解析后的 LDR 场景纹理（与 SwapChain 颜色格式一致）
    if (!createTex("UI Scene Color", rtvFormat, w, h, uiSceneColor_, uiSceneRTV_, uiSceneSRV_)) {
        return false;
    }

    // UI Blur 纹理（低分辨率 float，用于更平滑的模糊采样）
    if (!createTex("UI Blur A (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiBlurTexA_, uiBlurRTV_A_,
                   uiBlurSRV_A_)) {
        return false;
    }
    if (!createTex("UI Blur B (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiBlurTexB_, uiBlurRTV_B_,
                   uiBlurSRV_B_)) {
        return false;
    }
    if (!createTex("UI Blur C (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiBlurTexC_, uiBlurRTV_C_,
                   uiBlurSRV_C_)) {
        return false;
    }
    if (!createTex("UI Blur D (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiBlurTexD_, uiBlurRTV_D_,
                   uiBlurSRV_D_)) {
        return false;
    }

    // Acrylic 合成输出（同分辨率）
    if (!createTex("UI Acrylic Strong (1/6)", kOffscreenColorFormat, uiBlurW_, uiBlurH_, uiAcrylicStrong_,
                   uiAcrylicRTV_Strong_, uiAcrylicSRV_Strong_)) {
        return false;
    }
    if (!createTex("UI Acrylic Weak (1/12)", kOffscreenColorFormat, uiBlurW2_, uiBlurH2_, uiAcrylicWeak_,
                   uiAcrylicRTV_Weak_, uiAcrylicSRV_Weak_)) {
        return false;
    }

    // 噪点纹理（全分辨率、一次性上传；避免依赖 wrap sampler）
    {
        uiNoiseSRV_.Release();
        uiNoiseTex_.Release();

        std::vector<uint8_t> noise;
        noise.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);

        std::mt19937                       gen{1337u};
        std::uniform_int_distribution<int> rnd(0, 255);
        for (size_t i = 0; i < noise.size(); i += 4u) {
            const uint8_t v = static_cast<uint8_t>(rnd(gen));
            noise[i + 0u]   = v;
            noise[i + 1u]   = v;
            noise[i + 2u]   = v;
            noise[i + 3u]   = 255u;
        }

        TextureDesc texDesc{};
        texDesc.Name      = "UI Noise Texture";
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = w;
        texDesc.Height    = h;
        texDesc.MipLevels = 1;
        texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
        texDesc.BindFlags = BIND_SHADER_RESOURCE;
        texDesc.Usage     = USAGE_IMMUTABLE;

        TextureSubResData subRes{};
        subRes.pData  = noise.data();
        subRes.Stride = static_cast<Uint32>(w * 4u);

        TextureData texData{};
        texData.NumSubresources = 1;
        texData.pSubResources   = &subRes;

        device_->CreateTexture(texDesc, &texData, &uiNoiseTex_);
        if (uiNoiseTex_ == nullptr) {
            return false;
        }
        uiNoiseSRV_ = uiNoiseTex_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
        if (uiNoiseSRV_ == nullptr) {
            return false;
        }
    }

    return true;
}

Diligent::ITextureView* DiligentBackend::GetOrCreateLogControlIconSRV(
    bool pausedState /* true=resume icon, false=pause icon */) {
    if (device_ == nullptr) {
        return nullptr;
    }

    auto& tex = pausedState ? logResumeIconTex_ : logPauseIconTex_;
    auto& srv = pausedState ? logResumeIconSRV_ : logPauseIconSRV_;
    if (srv != nullptr) {
        return srv.RawPtr();
    }

    const int      px  = GeneratedIcons::kLogIconPx;
    const uint32_t rgb = GeneratedIcons::kLogIconRgb;
    const uint8_t* a   = pausedState ? GeneratedIcons::kLogResumeAlpha : GeneratedIcons::kLogPauseAlpha;
    if (px <= 0 || a == nullptr) {
        return nullptr;
    }

    const uint8_t r = static_cast<uint8_t>((rgb >> 16) & 0xFFu);
    const uint8_t g = static_cast<uint8_t>((rgb >> 8) & 0xFFu);
    const uint8_t b = static_cast<uint8_t>(rgb & 0xFFu);

    std::vector<uint8_t> rgba(static_cast<size_t>(px) * static_cast<size_t>(px) * 4u, 0u);
    for (int i = 0; i < px * px; i++) {
        rgba[static_cast<size_t>(i) * 4u + 0u] = r;
        rgba[static_cast<size_t>(i) * 4u + 1u] = g;
        rgba[static_cast<size_t>(i) * 4u + 2u] = b;
        rgba[static_cast<size_t>(i) * 4u + 3u] = a[i];
    }

    TextureDesc texDesc{};
    texDesc.Name      = pausedState ? "Log Resume Icon" : "Log Pause Icon";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = static_cast<Uint32>(px);
    texDesc.Height    = static_cast<Uint32>(px);
    texDesc.MipLevels = 1;
    texDesc.Format    = TEX_FORMAT_RGBA8_UNORM;
    texDesc.BindFlags = BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_IMMUTABLE;

    TextureSubResData subRes{};
    subRes.pData  = rgba.data();
    subRes.Stride = static_cast<Uint32>(px * 4u);

    TextureData texData{};
    texData.NumSubresources = 1;
    texData.pSubResources   = &subRes;

    device_->CreateTexture(texDesc, &texData, &tex);
    if (tex == nullptr) {
        return nullptr;
    }

    srv = tex->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    return srv.RawPtr();
}

void DiligentBackend::RenderUISceneForUI() {
    if (immediateContext_ == nullptr || !IsInitialized() || fullscreenQuadPSO_ == nullptr ||
        fullscreenQuadSRB_ == nullptr || offscreenSRV_ == nullptr || bloomSRV_B_ == nullptr || uiSceneRTV_ == nullptr) {
        return;
    }

    ITextureView* rtv = uiSceneRTV_.RawPtr();
    immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // 使用缓存的变量指针（避免每帧字符串查找）
    if (fullscreenTexVar_ != nullptr) {
        fullscreenTexVar_->Set(offscreenSRV_);
    }
    if (fullscreenBloomVar_ != nullptr) {
        fullscreenBloomVar_->Set(bloomSRV_B_);
    }

    immediateContext_->SetPipelineState(fullscreenQuadPSO_);
    immediateContext_->CommitShaderResources(fullscreenQuadSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                        SET_VERTEX_BUFFERS_FLAG_RESET);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = kDrawVerifyFlags;
    immediateContext_->Draw(draw);
}

void DiligentBackend::RenderUIBlur() {
    if (immediateContext_ == nullptr) {
        return;
    }
    if (uiSceneSRV_ == nullptr || uiBlurRTV_A_ == nullptr || uiBlurSRV_A_ == nullptr || uiBlurRTV_B_ == nullptr ||
        uiBlurSRV_B_ == nullptr || uiBlurRTV_C_ == nullptr || uiBlurSRV_C_ == nullptr || uiBlurRTV_D_ == nullptr ||
        uiBlurSRV_D_ == nullptr) {
        return;
    }
    if (bloomDownsamplePSO_ == nullptr || bloomDownsampleSRB_ == nullptr || bloomBlurPSO_ == nullptr ||
        bloomBlurSRB_ == nullptr || bloomBlurConstants_ == nullptr) {
        return;
    }
    if (uiBlurW_ == 0 || uiBlurH_ == 0) {
        return;
    }

    auto updateBlurCB = [&](float texelX, float texelY, float offset, float threshold) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomBlurConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BlurCB {
                float texelSize[2];
                float offset;
                float threshold;
            };
            auto* cb         = static_cast<BlurCB*>(mapped);
            cb->texelSize[0] = texelX;
            cb->texelSize[1] = texelY;
            cb->offset       = offset;
            cb->threshold    = threshold;
            immediateContext_->UnmapBuffer(bloomBlurConstants_, MAP_WRITE);
        }
    };

    // 使用缓存的变量指针（避免每帧字符串查找）
    // bloomDownTexVar_ 和 bloomBlurTexVar_ 在 CreateBloomPSO() 中初始化

    // --- 1) downsample: uiScene(full) -> uiBlurA(1/6), 不做 bright-pass ---
    {
        ITextureView* rtv = uiBlurRTV_A_.RawPtr();
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = static_cast<float>(uiBlurW_);
        vp.Height   = static_cast<float>(uiBlurH_);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp, 0, 0);

        const float texelX = (surfaceSize_.Width > 0) ? (1.0f / static_cast<float>(surfaceSize_.Width)) : 0.0f;
        const float texelY = (surfaceSize_.Height > 0) ? (1.0f / static_cast<float>(surfaceSize_.Height)) : 0.0f;
        updateBlurCB(texelX, texelY, 0.0f, 0.0f);

        if (bloomDownTexVar_ != nullptr) {
            bloomDownTexVar_->Set(uiSceneSRV_);
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // --- 2) Kawase blur ping-pong: uiBlurA <-> uiBlurB ---
    // blurStrength 滑条是 float：旧实现用 int 决定 iterations，导致“滑条无级、效果有级”。
    // 这里改为固定迭代次数 + 连续缩放 offset，让 blurStrength 真正连续生效。
    const float            blurStrength   = (appState_ != nullptr) ? appState_->ui.blurStrength : 2.0f;
    static constexpr float offsets[]      = {0.0f, 1.0f, 2.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    static constexpr int   kMaxIterations = static_cast<int>(sizeof(offsets) / sizeof(offsets[0])); // 8（偶数）
    const float            strength       = std::clamp(blurStrength, 0.0f, 5.0f);
    const float            scale          = strength / 5.0f; // 0..1
    auto                   scaledOffset   = [&](float base) -> float {
        // Shader: off = g_TexelSize * (g_Offset + 0.5)
        // 让 scale=0 时 off=0，scale=1 时保持旧行为：
        // g_Offset = scale*(base+0.5) - 0.5
        return scale * (base + 0.5f) - 0.5f;
    };
    const int iterations = kMaxIterations;

    const float texelX6 = 1.0f / static_cast<float>(uiBlurW_);
    const float texelY6 = 1.0f / static_cast<float>(uiBlurH_);

    for (int i = 1; i < iterations; ++i) {
        const bool    writeToB = (i % 2 == 1);
        ITextureView* outRTV   = writeToB ? uiBlurRTV_B_.RawPtr() : uiBlurRTV_A_.RawPtr();
        ITextureView* inSRV    = writeToB ? uiBlurSRV_A_.RawPtr() : uiBlurSRV_B_.RawPtr();

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(texelX6, texelY6, scaledOffset(offsets[i]), 0.0f);

        if (bloomBlurTexVar_ != nullptr) {
            bloomBlurTexVar_->Set(inSRV);
        }

        immediateContext_->SetPipelineState(bloomBlurPSO_);
        immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // --- 3) secondary (1/12): downsample uiBlurB(1/6) -> uiBlurC(1/12), 再做 2 次小 offset 模糊 ---
    if (uiBlurW2_ == 0 || uiBlurH2_ == 0) {
        return;
    }
    {
        ITextureView* rtv = uiBlurRTV_C_.RawPtr();
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp2{};
        vp2.TopLeftX = 0.0f;
        vp2.TopLeftY = 0.0f;
        vp2.Width    = static_cast<float>(uiBlurW2_);
        vp2.Height   = static_cast<float>(uiBlurH2_);
        vp2.MinDepth = 0.0f;
        vp2.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp2, 0, 0);

        updateBlurCB(texelX6, texelY6, 0.0f, 0.0f);
        if (bloomDownTexVar_ != nullptr) {
            bloomDownTexVar_->Set(uiBlurSRV_B_.RawPtr());
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    const float            texelX12           = 1.0f / static_cast<float>(uiBlurW2_);
    const float            texelY12           = 1.0f / static_cast<float>(uiBlurH2_);
    static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
    static constexpr int   secondaryIterations =
        static_cast<int>(sizeof(secondaryOffsets) / sizeof(secondaryOffsets[0])); // 偶数，最终结果落回 C

    for (int i = 0; i < secondaryIterations; ++i) {
        const bool    writeToD = (i % 2 == 0); // C->D->C...
        ITextureView* outRTV   = writeToD ? uiBlurRTV_D_.RawPtr() : uiBlurRTV_C_.RawPtr();
        ITextureView* inSRV    = writeToD ? uiBlurSRV_C_.RawPtr() : uiBlurSRV_D_.RawPtr();

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(texelX12, texelY12, scaledOffset(secondaryOffsets[i]), 0.0f);
        if (bloomBlurTexVar_ != nullptr) {
            bloomBlurTexVar_->Set(inSRV);
        }
        immediateContext_->SetPipelineState(bloomBlurPSO_);
        immediateContext_->CommitShaderResources(bloomBlurSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }
}

void DiligentBackend::RenderAcrylicComposite() {
    if (immediateContext_ == nullptr) {
        return;
    }
    if (appState_ != nullptr && !appState_->ui.blurEnabled) {
        return;
    }
    if (acrylicPSO_ == nullptr || acrylicSRB_ == nullptr || acrylicConstants_ == nullptr) {
        return;
    }
    if (uiAcrylicRTV_Strong_ == nullptr || uiAcrylicSRV_Strong_ == nullptr || uiAcrylicRTV_Weak_ == nullptr ||
        uiAcrylicSRV_Weak_ == nullptr) {
        return;
    }
    if (uiBlurSRV_B_ == nullptr || uiBlurSRV_C_ == nullptr) {
        return;
    }
    if (uiBlurW_ == 0 || uiBlurH_ == 0 || uiBlurW2_ == 0 || uiBlurH2_ == 0) {
        return;
    }

    const bool isDark = (appState_ != nullptr) ? appState_->ui.darkMode : true;

    auto updateCB = [&](float tintR, float tintG, float tintB, float baseOpacity, float saturation, float adaptive,
                        float exclusionStrength) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(acrylicConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct AcrylicCB {
                float Tint[4];
                float Params[4];
            };
            auto* cb    = static_cast<AcrylicCB*>(mapped);
            cb->Tint[0] = tintR;
            cb->Tint[1] = tintG;
            cb->Tint[2] = tintB;
            cb->Tint[3] = baseOpacity;

            cb->Params[0] = saturation;
            cb->Params[1] = adaptive;
            cb->Params[2] = isDark ? 1.0f : 0.0f;
            cb->Params[3] = exclusionStrength;

            immediateContext_->UnmapBuffer(acrylicConstants_, MAP_WRITE);
        }
    };

    // 使用缓存的变量指针（acrylicTexVar_ 在 CreateAcrylicPSO() 中初始化）

    auto drawComposite = [&](ITextureView* outRTV, uint32_t w, uint32_t h, ITextureView* inSRV) {
        if (outRTV == nullptr || inSRV == nullptr) {
            return;
        }

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        Viewport vp{};
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = static_cast<float>(w);
        vp.Height   = static_cast<float>(h);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        immediateContext_->SetViewports(1, &vp, 0, 0);

        if (acrylicTexVar_ != nullptr) {
            acrylicTexVar_->Set(inSRV);
        }

        immediateContext_->SetPipelineState(acrylicPSO_);
        immediateContext_->CommitShaderResources(acrylicSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    };

    // Strong Acrylic (1/6)：用于窗口背景
    {
        // 经验值：Acrylic 通常比原背景更“鲜艳”，并用较高 opacity 稳定可读性
        const float saturation = 1.35f;
        const float adaptive   = 0.35f;
        const float excl       = 1.0f;

        if (isDark) {
            updateCB(20.0f / 255.0f, 20.0f / 255.0f, 25.0f / 255.0f, 180.0f / 255.0f, saturation, adaptive, excl);
        } else {
            updateCB(245.0f / 255.0f, 245.0f / 255.0f, 255.0f / 255.0f, 150.0f / 255.0f, saturation, adaptive, excl);
        }
        drawComposite(uiAcrylicRTV_Strong_.RawPtr(), uiBlurW_, uiBlurH_, uiBlurSRV_B_.RawPtr());
    }

    // Weak Acrylic (1/12)：用于折叠区域/次级背景
    {
        const float saturation = 1.30f;
        const float adaptive   = 0.30f;
        const float excl       = 1.0f;

        if (isDark) {
            updateCB(35.0f / 255.0f, 35.0f / 255.0f, 40.0f / 255.0f, 160.0f / 255.0f, saturation, adaptive, excl);
        } else {
            updateCB(250.0f / 255.0f, 250.0f / 255.0f, 255.0f / 255.0f, 140.0f / 255.0f, saturation, adaptive, excl);
        }
        drawComposite(uiAcrylicRTV_Weak_.RawPtr(), uiBlurW2_, uiBlurH2_, uiBlurSRV_C_.RawPtr());
    }
}

} // namespace ParticleSaturn::Render
