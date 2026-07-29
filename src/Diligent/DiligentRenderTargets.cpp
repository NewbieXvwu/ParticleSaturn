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

// D-015 后续（cc8e4a）：离屏 RT/全屏四边形/清屏/离屏渲染/回拷，从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateFullscreenQuadPSO() {
    if (device_ == nullptr || !IsInitialized()) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> vs, ps;

    vs = PS_SHADER_FROM_BYTECODE(device_, backend_, "FullscreenQuad VS", SHADER_TYPE_VERTEX, FullscreenQuad_VS);
    ps = PS_SHADER_FROM_BYTECODE(device_, backend_, "FullscreenQuad PS", SHADER_TYPE_PIXEL, FullscreenQuad_PS);

    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "FullscreenQuad PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    // 根据当前模式确定 RTV 格式
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (useVkD3D12Interop_ && vkD3D12Interop_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }

    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = rtvFormat;
    // 当前阶段的离屏 RT 不带深度；PSO 也不绑定 DSV。
    psoCI.GraphicsPipeline.DSVFormat = TEX_FORMAT_UNKNOWN;

    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // 离屏合成：采样 HDR 纹理 + Bloom 纹理并做 tone mapping。
    // g_Texture 和 g_BloomTexture 使用 DYNAMIC 以便每帧可以更换（Resize 后需要更新）
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_PIXEL, "g_Texture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "g_BloomTexture", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_PIXEL, "BloomCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
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

    // Vulkan/GLSL 使用组合采样器 (sampler2D)，采样器名称直接用纹理名
    // D3D12/HLSL 使用分离采样器，采样器名称需要 "_sampler" 后缀
    const char* texSamplerName   = (backend_ == Backend::Vulkan) ? "g_Texture" : "g_Texture_sampler";
    const char* bloomSamplerName = (backend_ == Backend::Vulkan) ? "g_BloomTexture" : "g_BloomTexture_sampler";

    const ImmutableSamplerDesc imtblSamplers[] = {
        {SHADER_TYPE_PIXEL, texSamplerName, sampDesc},
        {SHADER_TYPE_PIXEL, bloomSamplerName, sampDesc},
    };
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = _countof(imtblSamplers);
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = imtblSamplers;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    fullscreenQuadPSO_.Release();
    fullscreenQuadSRB_.Release();
    CreateGraphicsPSO(device_, psoCI, &fullscreenQuadPSO_);
    if (fullscreenQuadPSO_ == nullptr) {
        return false;
    }

    // 创建 Bloom 常量缓冲
    if (bloomConstants_ == nullptr) {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Bloom Constants";
        cbDesc.Size           = 16; // float4: bloomStrength + padding
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        device_->CreateBuffer(cbDesc, nullptr, &bloomConstants_);
        if (bloomConstants_ == nullptr) {
            return false;
        }
    }

    // 绑定 BloomCB（STATIC 变量）
    if (auto* var = fullscreenQuadPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "BloomCB"); var != nullptr) {
        var->Set(bloomConstants_);
    }

    fullscreenQuadPSO_->CreateShaderResourceBinding(&fullscreenQuadSRB_, true);
    if (fullscreenQuadSRB_ != nullptr) {
        // 缓存热路径变量指针
        fullscreenTexVar_   = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_Texture");
        fullscreenBloomVar_ = fullscreenQuadSRB_->GetVariableByName(SHADER_TYPE_PIXEL, "g_BloomTexture");
    }
    return fullscreenQuadSRB_ != nullptr;
}

bool DiligentBackend::CreateOffscreenRenderTarget(SurfaceSize size) {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_)) {
        return false;
    }
    if (size.Width == 0 || size.Height == 0) {
        return false;
    }

    TextureDesc texDesc{};
    texDesc.Name      = "Offscreen Color";
    texDesc.Type      = RESOURCE_DIM_TEX_2D;
    texDesc.Width     = size.Width;
    texDesc.Height    = size.Height;
    texDesc.MipLevels = 1;
    // 与 OpenGL 旧版 FBO 对齐：R11G11B10F HDR（便于加法混合后在最终合成阶段做 tone mapping，避免过曝）。
    texDesc.Format    = kOffscreenColorFormat;
    texDesc.BindFlags = BIND_RENDER_TARGET | BIND_SHADER_RESOURCE;
    texDesc.Usage     = USAGE_DEFAULT;

    offscreenRTV_.Release();
    offscreenSRV_.Release();
    offscreenColor_.Release();

    device_->CreateTexture(texDesc, nullptr, &offscreenColor_);
    if (offscreenColor_ == nullptr) {
        return false;
    }

    offscreenRTV_ = offscreenColor_->GetDefaultView(TEXTURE_VIEW_RENDER_TARGET);
    offscreenSRV_ = offscreenColor_->GetDefaultView(TEXTURE_VIEW_SHADER_RESOURCE);
    if (offscreenSRV_ == nullptr) {
        return false;
    }
    return offscreenRTV_ != nullptr;
}

void DiligentBackend::UpdateFullscreenQuadBindings() {
    // g_Texture 和 g_BloomTexture 现在是 DYNAMIC 变量，不需要重新创建 SRB
    // 只需确保在 BlitOffscreenToBackBuffer 中每帧绑定正确的纹理即可
    // 此函数保留用于 PSO 创建后的初始化工作（如果需要）
}

void DiligentBackend::RenderClear() {
    if (!IsInitialized() || immediateContext_ == nullptr) {
        return;
    }

    ITextureView* pRTV = GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }

    // 深度缓冲：DirectComposition 模式下暂不使用深度缓冲（后续可扩展）
    ITextureView* pDSV = swapChain_ ? swapChain_->GetDepthBufferDSV() : nullptr;

    immediateContext_->SetRenderTargets(1, &pRTV, pDSV, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // 透明窗口模式下需要清为 alpha=0（否则 DWM 会把 client 区域当作不透明）。
    float clearColor[4] = {0.05f, 0.07f, 0.10f, 1.0f};
    if (appState_ != nullptr && appState_->backdrop.useTransparent) {
        clearColor[0] = 0.0f;
        clearColor[1] = 0.0f;
        clearColor[2] = 0.0f;
        clearColor[3] = 0.0f;
    }
    immediateContext_->ClearRenderTarget(pRTV, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    if (pDSV != nullptr) {
        immediateContext_->ClearDepthStencil(pDSV, CLEAR_DEPTH_FLAG, 1.0f, 0,
                                             RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

void DiligentBackend::RenderOffscreen() {
    if (immediateContext_ == nullptr) {
        return;
    }

    // 添加空指针检查，避免 Vulkan 上因无效 RTV 导致崩溃
    if (offscreenRTV_ == nullptr) {
        MD3::DebugLog::Instance().AddOnce("RenderOffscreen_NoRTV", MD3::LogLevel::Warn,
                                     "[RenderOffscreen] offscreenRTV_ is null, skipping");
        return;
    }

    ITextureView* pRTV = offscreenRTV_;
    immediateContext_->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 以实际离屏 RT 尺寸为准，避免 Resize/DPI 缩放导致的"像素尺寸 -> NDC"换算偏差（会直接影响星空/粒子"密度"观感）。
    uint32_t rtW = surfaceSize_.Width;
    uint32_t rtH = surfaceSize_.Height;
    if (offscreenColor_ != nullptr) {
        const auto& rtDesc = offscreenColor_->GetDesc();
        rtW                = rtDesc.Width;
        rtH                = rtDesc.Height;
    }

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(rtW);
    vp.Height   = static_cast<float>(rtH);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, rtW, rtH);

    // 星空背景：先清为黑色，再加法混合叠加星点（更接近 OpenGL 旧实现观感）。
    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    immediateContext_->ClearRenderTarget(pRTV, clearColor, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 星空点精灵（加法混合）
    if (starPSO_ != nullptr && starVB_ != nullptr && starSRB_ != nullptr && starCount_ > 0) {
        // 更新常量（view/proj/model + 视口 + 时间）
        const auto now   = std::chrono::steady_clock::now();
        const auto secsF = std::chrono::duration<float>(now - startTime_).count();

        {
            PVoid mapped = nullptr;
            immediateContext_->MapBuffer(starConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
            if (mapped != nullptr) {
                auto* cb = static_cast<StarConstants*>(mapped);

                const float aspect = rtH > 0 ? (static_cast<float>(rtW) / static_cast<float>(rtH)) : 1.0f;

                const Mat4Rows view  = LookAtRH({0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
                const Mat4Rows proj  = PerspectiveRH_OpenGL(1.047f, aspect, 1.0f, 10000.0f);
                const Mat4Rows model = RotationY(secsF * 0.005f);

                for (int c = 0; c < 4; ++c) {
                    cb->ViewRow0[c] = view.Row[0][c];
                    cb->ViewRow1[c] = view.Row[1][c];
                    cb->ViewRow2[c] = view.Row[2][c];
                    cb->ViewRow3[c] = view.Row[3][c];

                    cb->ProjRow0[c] = proj.Row[0][c];
                    cb->ProjRow1[c] = proj.Row[1][c];
                    cb->ProjRow2[c] = proj.Row[2][c];
                    cb->ProjRow3[c] = proj.Row[3][c];

                    cb->ModelRow0[c] = model.Row[0][c];
                    cb->ModelRow1[c] = model.Row[1][c];
                    cb->ModelRow2[c] = model.Row[2][c];
                    cb->ModelRow3[c] = model.Row[3][c];
                }

                cb->ViewportParams[0] = rtW > 0 ? (2.0f / static_cast<float>(rtW)) : 0.0f;
                cb->ViewportParams[1] = rtH > 0 ? (2.0f / static_cast<float>(rtH)) : 0.0f;
                cb->ViewportParams[2] = static_cast<float>(rtW);
                cb->ViewportParams[3] = static_cast<float>(rtH);

                cb->TimeParams[0] = secsF;
                immediateContext_->UnmapBuffer(starConstants_, MAP_WRITE);
            }
        }
        immediateContext_->SetPipelineState(starPSO_);

        IBuffer* pVBs[]    = {starVB_};
        Uint64   offsets[] = {0};
        immediateContext_->SetVertexBuffers(0, 1, pVBs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        immediateContext_->CommitShaderResources(starSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DrawAttribs starsDraw{};
        starsDraw.NumVertices = 6;
        uint32_t starLodCount = starCount_;
        if (appState_ != nullptr && appState_->render.pixelRatio < 0.85f) {
            // OpenGL 版：低 pixelRatio 时绘制 60% 星星
            starLodCount = static_cast<uint32_t>(static_cast<float>(kStarCountBase) * kStarLodRatio);
            if (starLodCount > starCount_) {
                starLodCount = starCount_;
            }
        }
        starsDraw.NumInstances = starLodCount;
        starsDraw.Flags        = kDrawVerifyFlags;
        immediateContext_->Draw(starsDraw);
    }

    // 土星粒子（阶段 3：先 CPU 初始化，渲染验证）
    if (particlePSO_ != nullptr && particleSRB_ != nullptr && particleConstants_ != nullptr && particleCount_ > 0) {
        const auto now   = std::chrono::steady_clock::now();
        const auto secsF = std::chrono::duration<float>(now - startTime_).count();

        // D-015 Phase B：相机动画（自动正弦 / 手部绝对姿态映射 + dt 相关平滑）已上移到
        // 共享 App::FrameCoordinator（外壳固定步长驱动，含灵敏度/反转/暂停），平滑结果写入
        // state.scene.*。后端只读取这三个量，本帧 dt / 是否有手来自 FrameContext。
        const float dt        = frameDeltaTime_;
        const bool  hasHand   = handTracked_;
        const float animScale = (appState_ != nullptr) ? appState_->scene.zoom : 1.0f;
        const float animRotX  = (appState_ != nullptr) ? appState_->scene.rotationX : 0.4f;
        const float animRotY  = (appState_ != nullptr) ? appState_->scene.rotationY : 0.0f;

        // 阶段 3（第 2 步）：接入 GPU ComputeSaturn（物理模拟）并用三缓冲轮转避免读写冲突。
        // uHandHas：1 表示有手，0 表示无手（只影响 compute 的交互分支）。
        SimulateParticles(dt, animScale, hasHand ? 1.0f : 0.0f);

        {
            PVoid mapped = nullptr;
            immediateContext_->MapBuffer(particleConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
            if (mapped != nullptr) {
                auto* cb = static_cast<StarConstants*>(mapped);

                const float aspect = rtH > 0 ? (static_cast<float>(rtW) / static_cast<float>(rtH)) : 1.0f;

                const Mat4Rows view = LookAtRH({0.0f, 0.0f, 100.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
                const Mat4Rows proj = PerspectiveRH_OpenGL(1.047f, aspect, 1.0f, 10000.0f);

                // 复刻 OpenGL 的角度逻辑：mSat = Rx(rotX) * Ry(rotY) * Rz(0.466)
                const float uScale = animScale;
                const float rotX   = animRotX;
                const float rotY   = animRotY;
                const float rotZ   = 0.466f;

                const Mat4Rows model = Mul(Mul(RotationX(rotX), RotationY(rotY)), RotationZ(rotZ));

                for (int c = 0; c < 4; ++c) {
                    cb->ViewRow0[c] = view.Row[0][c];
                    cb->ViewRow1[c] = view.Row[1][c];
                    cb->ViewRow2[c] = view.Row[2][c];
                    cb->ViewRow3[c] = view.Row[3][c];

                    cb->ProjRow0[c] = proj.Row[0][c];
                    cb->ProjRow1[c] = proj.Row[1][c];
                    cb->ProjRow2[c] = proj.Row[2][c];
                    cb->ProjRow3[c] = proj.Row[3][c];

                    cb->ModelRow0[c] = model.Row[0][c];
                    cb->ModelRow1[c] = model.Row[1][c];
                    cb->ModelRow2[c] = model.Row[2][c];
                    cb->ModelRow3[c] = model.Row[3][c];
                }

                cb->ViewportParams[0] = rtW > 0 ? (2.0f / static_cast<float>(rtW)) : 0.0f;
                cb->ViewportParams[1] = rtH > 0 ? (2.0f / static_cast<float>(rtH)) : 0.0f;
                cb->ViewportParams[2] = static_cast<float>(rtW);
                cb->ViewportParams[3] = static_cast<float>(rtH);

                cb->TimeParams[0] = secsF;

                // 对齐 OpenGL：pixelRatio / densityComp 由动态 LOD 或 UI 控制。
                const float pixelRatio =
                    (appState_ != nullptr && appState_->render.pixelRatio > 0.0f) ? appState_->render.pixelRatio : 1.0f;
                const float densityComp = (appState_ != nullptr) ? appState_->render.densityCompensation
                                                                 : ComputeDensityComp(particleCount_, pixelRatio);

                cb->RenderParams[0] = uScale;
                cb->RenderParams[1] = pixelRatio;
                cb->RenderParams[2] = static_cast<float>(rtH);
                cb->RenderParams[3] = densityComp;

                // Mesh Shader 需要 ParticleCount
                cb->ParticleCount = particleCount_;

                immediateContext_->UnmapBuffer(particleConstants_, MAP_WRITE);
            }
        }

        // 使用 Mesh Shader 或 Vertex Pulling 渲染粒子
        if (useMeshShaders_ && particleMeshPSO_ != nullptr && particleMeshSRB_ != nullptr) {
            // Mesh Shader 路径
            immediateContext_->SetPipelineState(particleMeshPSO_);

            // 绑定粒子缓冲
            if (auto* var = particleMeshSRB_->GetVariableByName(SHADER_TYPE_MESH, "g_Particles");
                var != nullptr && particleSRVs_[particleRenderIdx_] != nullptr) {
                var->Set(particleSRVs_[particleRenderIdx_]);
            }

            immediateContext_->CommitShaderResources(particleMeshSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            // Dispatch Mesh Shader：每组 32 粒子，总组数 = ceil(particleCount / 32)
            DrawMeshAttribs drawAttribs;
            drawAttribs.ThreadGroupCountX = (particleCount_ + 31) / 32;
            drawAttribs.ThreadGroupCountY = 1;
            drawAttribs.ThreadGroupCountZ = 1;
            drawAttribs.Flags             = kDrawVerifyFlags;
            immediateContext_->DrawMesh(drawAttribs);
        } else {
            // Vertex Pulling 回退路径
            immediateContext_->SetPipelineState(particlePSO_);
            // 不使用顶点缓冲（SV_VertexID/InstanceID 生成），但需要清掉之前的绑定状态。
            immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                SET_VERTEX_BUFFERS_FLAG_RESET);

            // 使用缓存的变量指针更新 DYNAMIC 变量 g_Particles（三缓冲轮转后指向新的渲染缓冲区）
            if (particleSRVs_[particleRenderIdx_] != nullptr && particleSRVVar_ != nullptr) {
                particleSRVVar_->Set(particleSRVs_[particleRenderIdx_]);
            }

            immediateContext_->CommitShaderResources(particleSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

            // 复刻 OpenGL：glDrawArraysIndirect(GL_POINTS, nullptr)
            if (particleIndirectArgs_ != nullptr) {
                DrawIndirectAttribs ia{};
                ia.pAttribsBuffer                   = particleIndirectArgs_;
                ia.Flags                            = kDrawVerifyFlags;
                ia.AttribsBufferStateTransitionMode = RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
                immediateContext_->DrawIndirect(ia);
            }
        }
    }
}

void DiligentBackend::BlitOffscreenToBackBuffer() {
    if (!IsInitialized() || immediateContext_ == nullptr || fullscreenQuadPSO_ == nullptr ||
        fullscreenQuadSRB_ == nullptr || offscreenSRV_ == nullptr) {
        return;
    }

    ITextureView* pBackBufferRTV = GetCurrentBackBufferRTV();
    if (pBackBufferRTV == nullptr) {
        return;
    }

    // 全屏合成：offscreen + bloom + tone mapping
    immediateContext_->SetRenderTargets(1, &pBackBufferRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口（Diligent 需要显式设置）
    Viewport vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    // 优化：当 UI Blur 启用时，uiSceneColor_ 已经包含了合成结果（offscreen + bloom + tone mapping），
    // 直接复用它，避免重复执行一遍完全相同的全屏合成 pass。
    const bool useUISceneAsSource = (appState_ != nullptr && appState_->ui.blurEnabled && uiSceneSRV_ != nullptr);

    // Bloom 强度（由 UI 控制）
    if (bloomConstants_ != nullptr) {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(bloomConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            struct BloomCB {
                float strength;
                float transparent;
                float isD3D11; // pad[0] -> isD3D11
                float pad;
            };

            auto* cb = static_cast<BloomCB*>(mapped);
            // 当复用 uiSceneColor_ 时，bloom 已经在 RenderUISceneForUI() 中处理过，这里设为 0 避免二次叠加
            cb->strength    = useUISceneAsSource ? 0.0f
                : ((appState_ != nullptr && appState_->render.bloomEnabled)
                       ? std::max(0.0f, appState_->render.bloomBlurStrength)
                       : 0.0f);
            cb->transparent = (appState_ != nullptr && appState_->backdrop.useTransparent) ? 1.0f : 0.0f;
            cb->isD3D11     = (backend_ == Backend::D3D11) ? 1.0f : 0.0f;
            cb->pad         = 0.0f;
            immediateContext_->UnmapBuffer(bloomConstants_, MAP_WRITE);
        }
    }

    // 使用缓存的变量指针绑定纹理
    if (fullscreenTexVar_ != nullptr) {
        // 优化：复用 UI 场景纹理（已包含 offscreen + bloom 合成结果）
        fullscreenTexVar_->Set(useUISceneAsSource ? uiSceneSRV_.RawPtr() : offscreenSRV_.RawPtr());
    }
    // Bloom：当复用 uiSceneColor_ 时，bloom 已内含，这里绑定相同纹理（shader 中 strength=0 不会叠加）
    if (fullscreenBloomVar_ != nullptr) {
        if (useUISceneAsSource) {
            // bloom 已在 uiSceneColor_ 中，绑定同一纹理作为占位
            fullscreenBloomVar_->Set(uiSceneSRV_.RawPtr());
        } else if (bloomSRV_B_ != nullptr) {
            fullscreenBloomVar_->Set(bloomSRV_B_);
        } else {
            fullscreenBloomVar_->Set(offscreenSRV_);
        }
    }

    immediateContext_->SetPipelineState(fullscreenQuadPSO_);
    immediateContext_->CommitShaderResources(fullscreenQuadSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DrawAttribs draw{};
    draw.NumVertices = 4;
    draw.Flags       = kDrawVerifyFlags;
    immediateContext_->Draw(draw);
}

} // namespace ParticleSaturn::Render
