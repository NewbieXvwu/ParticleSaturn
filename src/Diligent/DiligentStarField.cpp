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

// D-015 后续（cc8e4a）：星空场（星点实例缓冲 + PSO），从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateStarfieldBuffers(uint32_t starCount) {
    if (device_ == nullptr) {
        return false;
    }
    if (starCount == 0) {
        return false;
    }

    std::mt19937                          gen{1337u};
    std::uniform_real_distribution<float> rnd01(0.0f, 1.0f);

    const uint32_t kPalette[] = {0xE3DAC5u, 0xC9A070u, 0xE3DAC5u, 0xB08D55u};

    std::vector<StarInstance> stars;
    stars.resize(starCount);
    for (uint32_t i = 0; i < starCount; ++i) {
        // 复刻 OpenGL 旧实现：球壳分布
        const float r  = 400.0f + rnd01(gen) * 3000.0f;
        const float th = rnd01(gen) * 6.28318530718f;
        const float ph = std::acos(2.0f * rnd01(gen) - 1.0f);

        StarInstance v{};
        v.Pos[0] = r * std::sin(ph) * std::cos(th);
        v.Pos[1] = r * std::cos(ph);
        v.Pos[2] = r * std::sin(ph) * std::sin(th);

        HexToRGB(kPalette[i % 4], v.Color);

        v.Size = 1.0f + rnd01(gen) * 3.0f;
        v.Seed = rnd01(gen);

        stars[i] = v;
    }

    // Vertex buffer
    {
        BufferDesc vbDesc{};
        vbDesc.Name      = "Starfield VB";
        vbDesc.Usage     = USAGE_IMMUTABLE;
        vbDesc.BindFlags = BIND_VERTEX_BUFFER;
        vbDesc.Size      = static_cast<Uint32>(sizeof(StarInstance) * stars.size());

        BufferData vbData{};
        vbData.pData    = stars.data();
        vbData.DataSize = vbDesc.Size;

        starVB_.Release();
        device_->CreateBuffer(vbDesc, &vbData, &starVB_);
        if (starVB_ == nullptr) {
            return false;
        }
    }

    // Constant buffer (dynamic)
    {
        if (starConstants_ == nullptr) {
            BufferDesc cbDesc{};
            cbDesc.Name           = "Starfield Constants";
            cbDesc.Size           = (sizeof(StarConstants) + 255) & ~255;
            cbDesc.Usage          = USAGE_DYNAMIC;
            cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
            cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

            device_->CreateBuffer(cbDesc, nullptr, &starConstants_);
            if (starConstants_ == nullptr) {
                return false;
            }
        }
    }

    starCount_ = starCount;
    return true;
}

bool DiligentBackend::CreateStarfieldPSO() {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_) ||
        starConstants_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> vs, ps;

    vs = PS_SHADER_FROM_BYTECODE(device_, backend_, "Starfield VS", SHADER_TYPE_VERTEX, Star_VS);
    ps = PS_SHADER_FROM_BYTECODE(device_, backend_, "Starfield PS", SHADER_TYPE_PIXEL, Star_PS);

    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "Starfield PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets  = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]     = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat         = TEX_FORMAT_UNKNOWN;
    psoCI.GraphicsPipeline.PrimitiveTopology = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    psoCI.GraphicsPipeline.RasterizerDesc.CullMode      = CULL_MODE_NONE;
    psoCI.GraphicsPipeline.DepthStencilDesc.DepthEnable = False;

    auto& blendRT          = psoCI.GraphicsPipeline.BlendDesc.RenderTargets[0];
    blendRT.BlendEnable    = True;
    blendRT.SrcBlend       = BLEND_FACTOR_SRC_ALPHA;
    blendRT.DestBlend      = BLEND_FACTOR_ONE;
    blendRT.BlendOp        = BLEND_OPERATION_ADD;
    blendRT.SrcBlendAlpha  = BLEND_FACTOR_ONE;
    blendRT.DestBlendAlpha = BLEND_FACTOR_ONE;
    blendRT.BlendOpAlpha   = BLEND_OPERATION_ADD;

    const LayoutElement layoutElems[] = {
        LayoutElement{0, 0, 3, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{1, 0, 3, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{2, 0, 1, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
        LayoutElement{3, 0, 1, VT_FLOAT32, False, LAYOUT_ELEMENT_AUTO_OFFSET, LAYOUT_ELEMENT_AUTO_STRIDE,
                      INPUT_ELEMENT_FREQUENCY_PER_INSTANCE, 1},
    };
    psoCI.GraphicsPipeline.InputLayout.LayoutElements = layoutElems;
    psoCI.GraphicsPipeline.InputLayout.NumElements    = _countof(layoutElems);

    // 常量缓冲设为静态变量：每帧只更新 buffer 内容。
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "StarConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "StarConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    starPSO_.Release();
    CreateGraphicsPSO(device_, psoCI, &starPSO_);
    if (starPSO_ == nullptr) {
        return false;
    }

    if (auto* varVS = starPSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "StarConstants"); varVS != nullptr) {
        varVS->Set(starConstants_);
    } else {
        return false;
    }
    if (auto* varPS = starPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "StarConstants"); varPS != nullptr) {
        varPS->Set(starConstants_);
    } else {
        return false;
    }

    starSRB_.Release();
    starPSO_->CreateShaderResourceBinding(&starSRB_, true);
    return starSRB_ != nullptr;
}

} // namespace ParticleSaturn::Render
