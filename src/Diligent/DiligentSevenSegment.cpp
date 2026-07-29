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

// D-015 后续（cc8e4a）：七段数码管 FPS 显示，从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateSevenSegmentPSO() {
    if (device_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> vs, ps;

    vs = PS_SHADER_FROM_BYTECODE(device_, backend_, "SevenSegment VS", SHADER_TYPE_VERTEX, SevenSeg_VS);
    ps = PS_SHADER_FROM_BYTECODE(device_, backend_, "SevenSegment PS", SHADER_TYPE_PIXEL, SevenSeg_PS);

    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SevenSegment PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    // 顶点输入：float2 位置
    LayoutElement layoutElems[] = {
        {0, 0, 2, VT_FLOAT32, False},
    };
    psoCI.GraphicsPipeline.InputLayout.NumElements    = 1;
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;
    psoCI.GraphicsPipeline.PrimitiveTopology          = PRIMITIVE_TOPOLOGY_LINE_LIST;

    // 渲染目标格式
    TEXTURE_FORMAT rtvFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    if (useDCompSwapChain_) {
        rtvFormat = TEX_FORMAT_RGBA8_UNORM;
    } else if (swapChain_) {
        rtvFormat = swapChain_->GetDesc().ColorBufferFormat;
    }
    psoCI.GraphicsPipeline.NumRenderTargets = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]    = rtvFormat;
    psoCI.GraphicsPipeline.DSVFormat        = TEX_FORMAT_UNKNOWN; // 不使用深度

    // Alpha 混合（线条不透明，但保持一致性）
    auto& rt0                 = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    rt0.BlendEnable           = True;
    rt0.SrcBlend              = BLEND_FACTOR_SRC_ALPHA;
    rt0.DestBlend             = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOp               = BLEND_OPERATION_ADD;
    rt0.SrcBlendAlpha         = BLEND_FACTOR_ONE;
    rt0.DestBlendAlpha        = BLEND_FACTOR_INV_SRC_ALPHA;
    rt0.BlendOpAlpha          = BLEND_OPERATION_ADD;
    rt0.RenderTargetWriteMask = COLOR_MASK_ALL;

    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    // 资源布局
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "SevenSegCB", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.Variables    = vars;
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    CreateGraphicsPSO(device_, psoCI, &sevenSegPSO_);
    if (sevenSegPSO_ == nullptr) {
        return false;
    }

    // 创建常量缓冲
    BufferDesc cbDesc{};
    cbDesc.Name           = "SevenSegment Constants";
    cbDesc.Size           = 64 + 16 + 16; // mat4 + vec4 + vec4 (padding to 16-byte alignment)
    cbDesc.Usage          = USAGE_DYNAMIC;
    cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
    cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;
    device_->CreateBuffer(cbDesc, nullptr, &sevenSegConstants_);
    if (sevenSegConstants_ == nullptr) {
        return false;
    }

    // 绑定常量缓冲
    if (auto* var = sevenSegPSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "SevenSegCB"); var != nullptr) {
        var->Set(sevenSegConstants_);
    }

    sevenSegPSO_->CreateShaderResourceBinding(&sevenSegSRB_, true);
    return sevenSegSRB_ != nullptr;
}

bool DiligentBackend::CreateSevenSegmentBuffers() {
    if (device_ == nullptr) {
        return false;
    }

    // 标准化坐标 (0,0) 到 (1,1.8)，与 OpenGL 版一致
    const float w = 1.0f, h = 1.8f;
    const float p[6][2] = {{0, h}, {w, h}, {w, h / 2}, {w, 0}, {0, 0}, {0, h / 2}};

    for (int num = 0; num < 10; ++num) {
        std::vector<float> verts;
        auto               addLine = [&](int i1, int i2) {
            verts.push_back(p[i1][0]);
            verts.push_back(p[i1][1]);
            verts.push_back(p[i2][0]);
            verts.push_back(p[i2][1]);
        };

        if (kDigits[num][0]) {
            addLine(0, 1); // top
        }
        if (kDigits[num][1]) {
            addLine(1, 2); // top-right
        }
        if (kDigits[num][2]) {
            addLine(2, 3); // bottom-right
        }
        if (kDigits[num][3]) {
            addLine(3, 4); // bottom
        }
        if (kDigits[num][4]) {
            addLine(4, 5); // bottom-left
        }
        if (kDigits[num][5]) {
            addLine(5, 0); // top-left
        }
        if (kDigits[num][6]) {
            addLine(5, 2); // middle
        }

        sevenSegVertexCount_[num] = static_cast<uint32_t>(verts.size() / 2);

        BufferDesc vbDesc{};
        vbDesc.Name      = "SevenSegment VB";
        vbDesc.Size      = verts.size() * sizeof(float);
        vbDesc.Usage     = USAGE_IMMUTABLE;
        vbDesc.BindFlags = BIND_VERTEX_BUFFER;

        BufferData initData{};
        initData.pData    = verts.data();
        initData.DataSize = vbDesc.Size;

        sevenSegVB_[num].Release();
        device_->CreateBuffer(vbDesc, &initData, &sevenSegVB_[num]);
        if (sevenSegVB_[num] == nullptr) {
            return false;
        }
    }

    return true;
}

void DiligentBackend::RenderSevenSegmentFPS() {
    if (sevenSegPSO_ == nullptr || sevenSegSRB_ == nullptr || sevenSegConstants_ == nullptr) {
        return;
    }

    // 设置渲染目标为 SwapChain BackBuffer
    ITextureView* pRTV = GetCurrentBackBufferRTV();
    if (pRTV == nullptr) {
        return;
    }
    immediateContext_->SetRenderTargets(1, &pRTV, nullptr, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // 设置视口
    Viewport vp{};
    vp.Width    = static_cast<float>(surfaceSize_.Width);
    vp.Height   = static_cast<float>(surfaceSize_.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    immediateContext_->SetViewports(1, &vp, surfaceSize_.Width, surfaceSize_.Height);

    immediateContext_->SetPipelineState(sevenSegPSO_);

    // 计算正交投影矩阵（左下角原点，像素坐标）
    const float L = 0.0f;
    const float R = static_cast<float>(surfaceSize_.Width);
    const float T = static_cast<float>(surfaceSize_.Height);
    const float B = 0.0f;

    // 根据帧率选择颜色（与 OpenGL 版一致）
    float colorR, colorG, colorB;
    if (currentFps_ > 50.0f) {
        colorR = 0.3f;
        colorG = 1.0f;
        colorB = 0.3f; // 绿色
    } else if (currentFps_ > 30.0f) {
        colorR = 1.0f;
        colorG = 0.6f;
        colorB = 0.0f; // 橙色
    } else {
        colorR = 1.0f;
        colorG = 0.2f;
        colorB = 0.2f; // 红色
    }

    // FPS 数字渲染参数（右上角）
    const float numSize      = 20.0f;
    const float digitSpacing = numSize + 10.0f;
    float       xCursor      = static_cast<float>(surfaceSize_.Width) - 60.0f;
    const float yPos         = static_cast<float>(surfaceSize_.Height) - 40.0f;

    // 获取 FPS 数字
    int  displayFps = static_cast<int>(currentFps_);
    char fpsBuffer[8];
    int  fpsLen = snprintf(fpsBuffer, sizeof(fpsBuffer), "%d", displayFps);

    // 从右到左渲染每个数字
    for (int i = fpsLen - 1; i >= 0; --i) {
        int digit = fpsBuffer[i] - '0';
        if (digit < 0 || digit > 9) {
            continue;
        }
        if (sevenSegVB_[digit] == nullptr || sevenSegVertexCount_[digit] == 0) {
            continue;
        }

        // 更新常量缓冲
        struct SevenSegCB {
            float Projection[16];
            float Transform[4]; // x, y, scaleX, scaleY
            float Color[4];     // r, g, b, pad
        };

        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(sevenSegConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            auto* cb = static_cast<SevenSegCB*>(mapped);

            // 正交投影矩阵（列主序）
            // Diligent/D3D 使用列主序，需要转置
            std::memset(cb->Projection, 0, sizeof(cb->Projection));
            cb->Projection[0]  = 2.0f / (R - L);     // [0][0]
            cb->Projection[5]  = 2.0f / (T - B);     // [1][1]
            cb->Projection[10] = 1.0f;               // [2][2]
            cb->Projection[12] = -(R + L) / (R - L); // [3][0]
            cb->Projection[13] = -(T + B) / (T - B); // [3][1]
            cb->Projection[15] = 1.0f;               // [3][3]

            cb->Transform[0] = xCursor;
            cb->Transform[1] = yPos;
            cb->Transform[2] = numSize;
            cb->Transform[3] = numSize;

            cb->Color[0] = colorR;
            cb->Color[1] = colorG;
            cb->Color[2] = colorB;
            cb->Color[3] = 1.0f;

            immediateContext_->UnmapBuffer(sevenSegConstants_, MAP_WRITE);
        }

        immediateContext_->CommitShaderResources(sevenSegSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        // 绑定顶点缓冲
        IBuffer*     pBuffs[]  = {sevenSegVB_[digit]};
        const Uint64 offsets[] = {0};
        immediateContext_->SetVertexBuffers(0, 1, pBuffs, offsets, RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                                            SET_VERTEX_BUFFERS_FLAG_RESET);

        // 绘制
        DrawAttribs draw{};
        draw.NumVertices = sevenSegVertexCount_[digit];
        draw.Flags       = kDrawVerifyFlags;
        immediateContext_->Draw(draw);

        xCursor -= digitSpacing;
    }
}

} // namespace ParticleSaturn::Render
