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

// D-015 后续（cc8e4a）：Bloom（bright-pass + Kawase 模糊）纹理/PSO/渲染，从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateBloomPSO() {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> downVS, downPS, blurVS, blurPS;

    downVS = PS_SHADER_FROM_BYTECODE(device_, backend_, "BloomDownsample VS", SHADER_TYPE_VERTEX, BloomDownsample_VS);
    downPS = PS_SHADER_FROM_BYTECODE(device_, backend_, "BloomDownsample PS", SHADER_TYPE_PIXEL, BloomDownsample_PS);
    blurVS = PS_SHADER_FROM_BYTECODE(device_, backend_, "BloomBlur VS", SHADER_TYPE_VERTEX, BloomBlur_VS);
    blurPS = PS_SHADER_FROM_BYTECODE(device_, backend_, "BloomBlur PS", SHADER_TYPE_PIXEL, BloomBlur_PS);

    if (downVS == nullptr || downPS == nullptr || blurVS == nullptr || blurPS == nullptr) {
        return false;
    }

    // 常量缓冲（BlurCB）：float2 texelSize + float offset + float threshold
    if (bloomBlurConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Bloom Blur Constants";
        cbDesc.Size           = 16;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
        device_->CreateBuffer(cbDesc, nullptr, &bloomBlurConstants_);
        if (bloomBlurConstants_ == nullptr) {
            return false;
        }
    }

    SamplerDesc sampDesc{};
    sampDesc.MinFilter = FILTER_TYPE_LINEAR;
    sampDesc.MagFilter = FILTER_TYPE_LINEAR;
    sampDesc.MipFilter = FILTER_TYPE_LINEAR;
    sampDesc.AddressU  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV  = TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW  = TEXTURE_ADDRESS_CLAMP;

    // Vulkan/GLSL 使用组合采样器 (sampler2D)，采样器名称直接用纹理名 "g_Texture"
    // D3D12/HLSL 使用分离采样器，采样器名称需要 "_sampler" 后缀
    const char* samplerName = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";

    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, samplerName, sampDesc},
    };

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "BlurCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };

    auto createPso = [&](const char* name, IShader* vs, IShader* ps, RefCntAutoPtr<IPipelineState>& outPso,
                         RefCntAutoPtr<IShaderResourceBinding>& outSrb) -> bool {
        GraphicsPipelineStateCreateInfo psoCI{};
        psoCI.PSODesc.Name         = name;
        psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

        // 输出到 Bloom 纹理（R11G11B10F），不需要 DSV
        psoCI.GraphicsPipeline.NumRenderTargets = 1;
        psoCI.GraphicsPipeline.RTVFormats[0]    = kOffscreenColorFormat;
        psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN;

        psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
        psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

        psoCI.PSODesc.ResourceLayout.NumVariables         = _countof(vars);
        psoCI.PSODesc.ResourceLayout.Variables            = vars;
        psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
        psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

        psoCI.pVS = vs;
        psoCI.pPS = ps;

        outPso.Release();
        outSrb.Release();
        CreateGraphicsPSO(device_, psoCI, &outPso);
        if (outPso == nullptr) {
            return false;
        }

        if (auto* cbVar = outPso->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BlurCB"); cbVar != nullptr) {
            cbVar->Set(bloomBlurConstants_);
        } else {
            return false;
        }

        outPso->CreateShaderResourceBinding(&outSrb, true);
        return outSrb != nullptr;
    };

    if (!createPso("Bloom Downsample PSO", downVS, downPS, bloomDownsamplePSO_, bloomDownsampleSRB_)) {
        return false;
    }
    if (!createPso("Bloom Blur PSO", blurVS, blurPS, bloomBlurPSO_, bloomBlurSRB_)) {
        return false;
    }

    // 缓存热路径变量指针
    if (bloomDownsampleSRB_ != nullptr) {
        bloomDownTexVar_ = bloomDownsampleSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    }
    if (bloomBlurSRB_ != nullptr) {
        bloomBlurTexVar_ = bloomBlurSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
    }

    return true;
}

bool DiligentBackend::CreateBloomTextures(SurfaceSize size) {
    if (device_ == nullptr) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return true;
    }

    // 对齐 OpenGL 的玻璃模糊分辨率：1/6
    const uint32_t w = std::max(1u, size.Width / 6u);
    const uint32_t h = std::max(1u, size.Height / 6u);

    // 次级模糊分辨率：1/12（用于折叠区域 Acrylic 效果）
    const uint32_t w2 = std::max(1u, size.Width / 12u);
    const uint32_t h2 = std::max(1u, size.Height / 12u);

    const bool sizeChanged = (bloomW_ != w || bloomH_ != h || bloomW2_ != w2 || bloomH2_ != h2);
    if (!sizeChanged && bloomTexA_ != nullptr && bloomTexB_ != nullptr && bloomTexC_ != nullptr &&
        bloomTexD_ != nullptr) {
        return true;
    }

    bloomW_  = w;
    bloomH_  = h;
    bloomW2_ = w2;
    bloomH2_ = h2;

    auto createTex = [&](const char* name, uint32_t texW, uint32_t texH, RefCntAutoPtr<ITexture>& outTex,
                         RefCntAutoPtr<ITextureView>& outRTV, RefCntAutoPtr<ITextureView>& outSRV) -> bool {
        TextureDesc texDesc{};
        texDesc.Name      = name;
        texDesc.Type      = RESOURCE_DIM_TEX_2D;
        texDesc.Width     = texW;
        texDesc.Height    = texH;
        texDesc.MipLevels = 1;
        texDesc.Format    = kOffscreenColorFormat;
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

    // 1/6 分辨率纹理（窗口背景强模糊）
    if (!createTex("Bloom Tex A", bloomW_, bloomH_, bloomTexA_, bloomRTV_A_, bloomSRV_A_)) {
        return false;
    }
    if (!createTex("Bloom Tex B", bloomW_, bloomH_, bloomTexB_, bloomRTV_B_, bloomSRV_B_)) {
        return false;
    }

    // 1/12 分辨率纹理（折叠区域弱模糊）
    if (!createTex("Bloom Tex C", bloomW2_, bloomH2_, bloomTexC_, bloomRTV_C_, bloomSRV_C_)) {
        return false;
    }
    if (!createTex("Bloom Tex D", bloomW2_, bloomH2_, bloomTexD_, bloomRTV_D_, bloomSRV_D_)) {
        return false;
    }

    return true;
}

void DiligentBackend::RenderBloom() {
    if (immediateContext_ == nullptr || device_ == nullptr) {
        return;
    }
    if (offscreenSRV_ == nullptr || bloomRTV_A_ == nullptr || bloomSRV_A_ == nullptr || bloomRTV_B_ == nullptr ||
        bloomSRV_B_ == nullptr) {
        return;
    }
    if (bloomDownsamplePSO_ == nullptr || bloomDownsampleSRB_ == nullptr || bloomBlurPSO_ == nullptr ||
        bloomBlurSRB_ == nullptr || bloomBlurConstants_ == nullptr) {
        return;
    }
    if (bloomW_ == 0 || bloomH_ == 0) {
        return;
    }

    // 视口设为 Bloom 分辨率
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(bloomW_);
    vp.Height   = static_cast<float>(bloomH_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, bloomW_, bloomH_);

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

    // 使用缓存的变量指针（bloomDownTexVar_ 和 bloomBlurTexVar_ 在 CreateBloomPSO() 中初始化）

    // Pass 0: bright-pass downsample（offscreen -> bloomA）
    {
        ITextureView* rtv = bloomRTV_A_;
        immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // offscreen texel size
        uint32_t srcW = surfaceSize_.Width;
        uint32_t srcH = surfaceSize_.Height;
        if (offscreenColor_ != nullptr) {
            const auto& desc = offscreenColor_->GetDesc();
            srcW             = desc.Width;
            srcH             = desc.Height;
        }
        const float texelX = (srcW > 0) ? (1.0f / static_cast<float>(srcW)) : 0.0f;
        const float texelY = (srcH > 0) ? (1.0f / static_cast<float>(srcH)) : 0.0f;
        updateBlurCB(texelX, texelY, 0.0f, 1.0f);

        if (bloomDownTexVar_ != nullptr) {
            bloomDownTexVar_->Set(offscreenSRV_);
        }

        immediateContext_->SetPipelineState(bloomDownsamplePSO_);
        immediateContext_->CommitShaderResources(bloomDownsampleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // 无 VB，使用 SV_VertexID/gl_VertexIndex
        immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);
        DrawAttribs draw{};
        draw.NumVertices = 4;
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);
    }

    // Pass 1..N: Kawase blur ping-pong（bloomA <-> bloomB）
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

    const float bloomTexelX = 1.0f / static_cast<float>(bloomW_);
    const float bloomTexelY = 1.0f / static_cast<float>(bloomH_);

    for (int i = 1; i < iterations; ++i) {
        const bool    writeToB = (i % 2 == 1);
        ITextureView* outRTV   = writeToB ? bloomRTV_B_ : bloomRTV_A_;
        ITextureView* inSRV    = writeToB ? bloomSRV_A_ : bloomSRV_B_;

        immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        updateBlurCB(bloomTexelX, bloomTexelY, scaledOffset(offsets[i]), 0.0f);

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

    // ========== 次级模糊 (1/12 分辨率，用于折叠区域 Acrylic 效果) ==========
    // 从 bloomSRV_B_ (1/6) 降采样到 bloomTexC_ (1/12)，再做 2 次模糊。
    // 注意：不能 in-place（同一纹理同时 RTV+SRV）——在 D3D12/Vulkan 下属于未定义/不允许行为。
    if (bloomTexC_ != nullptr && bloomRTV_C_ != nullptr && bloomSRV_C_ != nullptr && bloomTexD_ != nullptr &&
        bloomRTV_D_ != nullptr && bloomSRV_D_ != nullptr) {
        const float texelX2 = 1.0f / static_cast<float>(bloomW2_);
        const float texelY2 = 1.0f / static_cast<float>(bloomH2_);

        // 第一步：从 1/6 降采样到 1/12（使用 downsample shader）
        {
            ITextureView* rtv = bloomRTV_C_.RawPtr();
            immediateContext_->SetRenderTargets(1, &rtv, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            Viewport vp2{};
            vp2.TopLeftX = 0.0f;
            vp2.TopLeftY = 0.0f;
            vp2.Width    = static_cast<float>(bloomW2_);
            vp2.Height   = static_cast<float>(bloomH2_);
            vp2.MinDepth = 0.0f;
            vp2.MaxDepth = 1.0f;
            immediateContext_->SetViewports(1, &vp2, 0, 0);

            // 使用 1/6 纹理作为输入
            if (bloomDownTexVar_ != nullptr) {
                bloomDownTexVar_->Set(bloomSRV_B_.RawPtr());
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

        // 第二步：2 次模糊迭代（使用较小的 offset）
        // 由于分辨率更低，使用较小的 offset 值。iterations 设为偶数，确保最终结果落在 bloomTexC_ 中。
        static constexpr float secondaryOffsets[] = {0.5f, 1.0f};
        static constexpr int   secondaryIterations =
            static_cast<int>(sizeof(secondaryOffsets) / sizeof(secondaryOffsets[0]));

        for (int i = 0; i < secondaryIterations; ++i) {
            const bool    writeToD = (i % 2 == 0); // C->D->C...
            ITextureView* outRTV   = writeToD ? bloomRTV_D_.RawPtr() : bloomRTV_C_.RawPtr();
            ITextureView* inSRV    = writeToD ? bloomSRV_C_.RawPtr() : bloomSRV_D_.RawPtr();

            immediateContext_->SetRenderTargets(1, &outRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            updateBlurCB(texelX2, texelY2, scaledOffset(secondaryOffsets[i]), 0.0f);

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
}

} // namespace ParticleSaturn::Render
