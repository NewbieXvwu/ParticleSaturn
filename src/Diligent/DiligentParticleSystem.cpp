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

// D-015 后续（cc8e4a）：粒子系统（缓冲/PSO/Mesh/Compute/CPU 初始化/模拟），从 DiligentBackend.cpp 拆出。
namespace ParticleSaturn::Render {

using namespace Diligent;
using namespace detail;

bool DiligentBackend::CreateParticleBuffers(uint32_t maxParticles) {
    if (device_ == nullptr || immediateContext_ == nullptr) {
        SetLastError(L"CreateParticleBuffers: device/context 为空。");
        return false;
    }
    if (maxParticles == 0) {
        SetLastError(L"CreateParticleBuffers: maxParticles=0。");
        return false;
    }

    // OpenGL 版在 ComputeInitSaturn 里用 time(0) 作为随机种子（uSeed）。
    // Diligent 版这里也对齐：避免环过于“统计学完美对称”，导致即使在公转也很难被肉眼感知。
    const uint32_t seed         = static_cast<uint32_t>(std::time(nullptr));
    const auto     cpuParticles = InitSaturnParticlesCPU(maxParticles, seed);
    if (cpuParticles.empty()) {
        SetLastError(L"CreateParticleBuffers: CPU 初始化粒子数组为空。");
        return false;
    }

    const Uint64 bufferSize = static_cast<Uint64>(sizeof(SaturnParticle)) * static_cast<Uint64>(cpuParticles.size());

    for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
        BufferDesc bufDesc{};
        bufDesc.Name              = "Saturn Particles";
        bufDesc.Size              = bufferSize;
        bufDesc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        bufDesc.Usage             = USAGE_DEFAULT;
        bufDesc.Mode              = BUFFER_MODE_STRUCTURED;
        bufDesc.ElementByteStride = sizeof(SaturnParticle);

        BufferData initData{};
        initData.pData    = cpuParticles.data();
        initData.DataSize = bufferSize;

        particleUAVs_[i].Release();
        particleSRVs_[i].Release();
        particleBuffers_[i].Release();

        device_->CreateBuffer(bufDesc, &initData, &particleBuffers_[i]);
        if (particleBuffers_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Saturn Particles) 失败（可能显存不足）。");
            return false;
        }

        particleSRVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE);
        particleUAVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
        if (particleSRVs_[i] == nullptr || particleUAVs_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffers: 获取粒子 SRV/UAV 失败。");
            return false;
        }
    }

    // 常量缓冲（每帧更新）
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Constants";
        cbDesc.Size           = (sizeof(StarConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleConstants_);
        if (particleConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Particle Constants) 失败。");
            return false;
        }
    }

    // Compute 常量缓冲（每帧更新）
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Compute Constants";
        cbDesc.Size           = (sizeof(ParticleComputeConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleComputeConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleComputeConstants_);
        if (particleComputeConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Particle Compute Constants) 失败。");
            return false;
        }
    }

    // Indirect draw args（复刻 OpenGL 的 glDrawArraysIndirect）：4x uint32
    // { NumVertices, NumInstances, StartVertexLocation, FirstInstanceLocation }
    {
        uint32_t args[4] = {6u, maxParticles, 0u, 0u};

        BufferDesc bufDesc{};
        bufDesc.Name      = "Particle Indirect Draw Args";
        bufDesc.Size      = sizeof(args);
        bufDesc.BindFlags = BIND_INDIRECT_DRAW_ARGS;
        // 注意：Indirect args 必须是 GPU 可读的缓冲；不要用可映射的动态缓冲（部分后端会直接创建失败）。
        // 后续如果要做动态 LOD，需要更新 args 时，用 IDeviceContext::UpdateBuffer() 写入即可。
        bufDesc.Usage          = USAGE_DEFAULT;
        bufDesc.CPUAccessFlags = CPU_ACCESS_NONE;

        BufferData init{};
        init.pData    = args;
        init.DataSize = sizeof(args);

        particleIndirectArgs_.Release();
        device_->CreateBuffer(bufDesc, &init, &particleIndirectArgs_);
        if (particleIndirectArgs_ == nullptr) {
            SetLastError(L"CreateParticleBuffers: CreateBuffer(Indirect Draw Args) 失败。");
            return false;
        }

        // D3D12: 显式将间接参数缓冲区转换到正确的初始状态
        StateTransitionDesc barrier{};
        barrier.pResource      = particleIndirectArgs_;
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
        barrier.NewState       = RESOURCE_STATE_INDIRECT_ARGUMENT;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    particleCount_ = maxParticles;
    // 使用正确的三缓冲初始化：确保 render、read、write 指向三个不同的缓冲区
    // 这样可以避免任何读写冲突
    // - buffer[0]：初始用于 read（计算输入）
    // - buffer[1]：初始用于 write（计算输出）
    // - buffer[2]：初始用于 render（渲染）
    particleRenderIdx_ = 2;
    particleReadIdx_   = 0;
    particleWriteIdx_  = 1;

    // Vulkan 修复：显式将所有粒子缓冲区转换到正确的初始资源状态。
    // 在 Vulkan 下，新创建的缓冲区处于 RESOURCE_STATE_UNKNOWN 状态，
    // 必须在首次使用前转换到正确状态，否则会导致验证层错误或崩溃。
    // - renderIdx 缓冲区将用于渲染（SRV 读取）
    // - readIdx 缓冲区将用于计算输入（SRV 读取）
    // - writeIdx 缓冲区将用于计算输出（UAV 写入）
    {
        StateTransitionDesc barriers[kParticleBufferCount] = {};
        for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
            barriers[i].pResource      = particleBuffers_[i];
            barriers[i].OldState       = RESOURCE_STATE_UNKNOWN;
            barriers[i].NewState       = RESOURCE_STATE_SHADER_RESOURCE;
            barriers[i].TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
            barriers[i].Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        }
        immediateContext_->TransitionResourceStates(kParticleBufferCount, barriers);
    }

    return true;
}

bool DiligentBackend::CreateParticleInitPSO() {
    if (device_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> cs;

    if (backend_ == Backend::Vulkan) {
        cs = CreateShaderFromBytecode(device_, "SaturnInit CS", SHADER_TYPE_COMPUTE, SaturnInit_CS_SPIRV,
                                      sizeof(SaturnInit_CS_SPIRV));
    } else {
        // D3D11/D3D12 use DXBC
        cs = CreateShaderFromBytecode(device_, "SaturnInit CS", SHADER_TYPE_COMPUTE, SaturnInit_CS_DXBC,
                                      sizeof(SaturnInit_CS_DXBC));
    }

    if (cs == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleInitPSO] Compute shader creation failed");
        return false;
    }

    ComputePipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name         = "Saturn Particle Init PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;
    psoCI.pCS                  = cs;

    // 资源签名：UAV + Constants
    ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_COMPUTE, "g_ParticlesOut", SHADER_RESOURCE_VARIABLE_TYPE_MUTABLE},
    };
    psoCI.PSODesc.ResourceLayout.Variables    = vars;
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);

    particleInitPSO_.Release();
    particleInitSRB_.Release();
    CreateComputePSO(device_, psoCI, &particleInitPSO_);
    if (particleInitPSO_ == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleInitPSO] CreateComputePipelineState failed");
        return false;
    }

    // 创建常量缓冲
    {
        struct InitConstants {
            uint32_t particleCount;
            uint32_t seed;
            float    radius;
            float    _pad;
        };

        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Init Constants";
        cbDesc.Size           = (sizeof(InitConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleInitConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleInitConstants_);
        if (particleInitConstants_ == nullptr) {
            MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleInitPSO] CreateBuffer(Init Constants) failed");
            return false;
        }
    }

    // 绑定常量
    auto var = particleInitPSO_->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "InitConstants");
    if (var != nullptr) {
        var->Set(particleInitConstants_);
    } else {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error,
                                 "[CreateParticleInitPSO] GetStaticVariableByName(InitConstants) returned nullptr");
        return false;
    }

    particleInitPSO_->CreateShaderResourceBinding(&particleInitSRB_, true);
    if (particleInitSRB_ == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleInitPSO] CreateShaderResourceBinding failed");
        return false;
    }

    return true;
}

bool DiligentBackend::CreateParticleBuffersGPU(uint32_t maxParticles) {
    if (device_ == nullptr || immediateContext_ == nullptr) {
        SetLastError(L"CreateParticleBuffersGPU: device/context 为空。");
        return false;
    }
    if (maxParticles == 0) {
        SetLastError(L"CreateParticleBuffersGPU: maxParticles=0。");
        return false;
    }

    const Uint64 bufferSize = static_cast<Uint64>(sizeof(SaturnParticle)) * static_cast<Uint64>(maxParticles);

    // 创建空的粒子缓冲区（不初始化数据）
    for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
        BufferDesc bufDesc{};
        bufDesc.Name              = "Saturn Particles";
        bufDesc.Size              = bufferSize;
        bufDesc.BindFlags         = BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS;
        bufDesc.Usage             = USAGE_DEFAULT;
        bufDesc.Mode              = BUFFER_MODE_STRUCTURED;
        bufDesc.ElementByteStride = sizeof(SaturnParticle);

        particleUAVs_[i].Release();
        particleSRVs_[i].Release();
        particleBuffers_[i].Release();

        device_->CreateBuffer(bufDesc, nullptr, &particleBuffers_[i]); // 无初始数据
        if (particleBuffers_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffersGPU: CreateBuffer(Saturn Particles) 失败（可能显存不足）。");
            return false;
        }

        particleSRVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_SHADER_RESOURCE);
        particleUAVs_[i] = particleBuffers_[i]->GetDefaultView(BUFFER_VIEW_UNORDERED_ACCESS);
        if (particleSRVs_[i] == nullptr || particleUAVs_[i] == nullptr) {
            SetLastError(L"CreateParticleBuffersGPU: 获取粒子 SRV/UAV 失败。");
            return false;
        }
    }

    // 创建初始化 PSO
    if (!CreateParticleInitPSO()) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Warn,
                                 "[CreateParticleBuffersGPU] CreateParticleInitPSO failed, falling back to CPU");
        return false;
    }

    // 使用 GPU Compute Shader 初始化所有粒子缓冲区
    const uint32_t seed = static_cast<uint32_t>(std::time(nullptr));

    struct InitConstants {
        uint32_t particleCount;
        uint32_t seed;
        float    radius;
        float    _pad;
    };

    InitConstants initConst{};
    initConst.particleCount = maxParticles;
    initConst.seed          = seed;
    initConst.radius        = 18.0f;

    {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(particleInitConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            *static_cast<InitConstants*>(mapped) = initConst;
            immediateContext_->UnmapBuffer(particleInitConstants_, MAP_WRITE);
        }
    }

    // 对每个缓冲区执行初始化
    for (uint32_t i = 0; i < kParticleBufferCount; ++i) {
        // 转换缓冲区到 UAV 状态
        StateTransitionDesc barrier{};
        barrier.pResource      = particleBuffers_[i];
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
        barrier.NewState       = RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);

        // 绑定输出缓冲区
        auto outVar = particleInitSRB_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_ParticlesOut");
        if (outVar != nullptr) {
            outVar->Set(particleUAVs_[i]);
        }

        immediateContext_->SetPipelineState(particleInitPSO_);
        immediateContext_->CommitShaderResources(particleInitSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

        DispatchComputeAttribs dispatchAttribs;
        dispatchAttribs.ThreadGroupCountX = (maxParticles + 255) / 256;
        dispatchAttribs.ThreadGroupCountY = 1;
        dispatchAttribs.ThreadGroupCountZ = 1;
        immediateContext_->DispatchCompute(dispatchAttribs);

        // 转换回 SRV 状态
        barrier.OldState = RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.NewState = RESOURCE_STATE_SHADER_RESOURCE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    // 等待 GPU 完成初始化
    immediateContext_->Flush();

    // 常量缓冲（与 CPU 版本相同）
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Constants";
        cbDesc.Size           = (sizeof(StarConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleConstants_);
        if (particleConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffersGPU: CreateBuffer(Particle Constants) 失败。");
            return false;
        }
    }

    // Compute 常量缓冲
    {
        BufferDesc cbDesc{};
        cbDesc.Name           = "Particle Compute Constants";
        cbDesc.Size           = (sizeof(ParticleComputeConstants) + 255) & ~255;
        cbDesc.Usage          = USAGE_DYNAMIC;
        cbDesc.BindFlags      = BIND_UNIFORM_BUFFER;
        cbDesc.CPUAccessFlags = CPU_ACCESS_WRITE;

        particleComputeConstants_.Release();
        device_->CreateBuffer(cbDesc, nullptr, &particleComputeConstants_);
        if (particleComputeConstants_ == nullptr) {
            SetLastError(L"CreateParticleBuffersGPU: CreateBuffer(Particle Compute Constants) 失败。");
            return false;
        }
    }

    // Indirect draw args
    {
        uint32_t args[4] = {6u, maxParticles, 0u, 0u};

        BufferDesc bufDesc{};
        bufDesc.Name           = "Particle Indirect Draw Args";
        bufDesc.Size           = sizeof(args);
        bufDesc.BindFlags      = BIND_INDIRECT_DRAW_ARGS;
        bufDesc.Usage          = USAGE_DEFAULT;
        bufDesc.CPUAccessFlags = CPU_ACCESS_NONE;

        BufferData init{};
        init.pData    = args;
        init.DataSize = sizeof(args);

        particleIndirectArgs_.Release();
        device_->CreateBuffer(bufDesc, &init, &particleIndirectArgs_);
        if (particleIndirectArgs_ == nullptr) {
            SetLastError(L"CreateParticleBuffersGPU: CreateBuffer(Indirect Draw Args) 失败。");
            return false;
        }

        StateTransitionDesc barrier{};
        barrier.pResource      = particleIndirectArgs_;
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
        barrier.NewState       = RESOURCE_STATE_INDIRECT_ARGUMENT;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    particleCount_     = maxParticles;
    particleRenderIdx_ = 2;
    particleReadIdx_   = 0;
    particleWriteIdx_  = 1;

    MD3::DebugLog::Instance().Add(MD3::LogLevel::Info, "[GPU] Particle initialization completed on GPU (" +
                                                 std::to_string(maxParticles) + " particles)");

    return true;
}

bool DiligentBackend::CreateParticlePSO() {
    if (device_ == nullptr || (swapChain_ == nullptr && !useDCompSwapChain_ && !useVkD3D12Interop_) ||
        particleConstants_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> vs, ps;

    if (backend_ == Backend::Vulkan) {
        vs = CreateShaderFromBytecode(device_, "SaturnParticle VS", SHADER_TYPE_VERTEX, SaturnParticle_VS_SPIRV,
                                      sizeof(SaturnParticle_VS_SPIRV));
        ps = CreateShaderFromBytecode(device_, "SaturnParticle PS", SHADER_TYPE_PIXEL, SaturnParticle_PS_SPIRV,
                                      sizeof(SaturnParticle_PS_SPIRV));
    } else {
        // D3D11/D3D12 use DXBC
        vs = CreateShaderFromBytecode(device_, "SaturnParticle VS", SHADER_TYPE_VERTEX, SaturnParticle_VS_DXBC,
                                      sizeof(SaturnParticle_VS_DXBC));
        ps = CreateShaderFromBytecode(device_, "SaturnParticle PS", SHADER_TYPE_PIXEL, SaturnParticle_PS_DXBC,
                                      sizeof(SaturnParticle_PS_DXBC));
    }

    if (vs == nullptr || ps == nullptr) {
        return false;
    }

    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnParticle PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_GRAPHICS;

    psoCI.GraphicsPipeline.NumRenderTargets             = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]                = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat                    = TEX_FORMAT_UNKNOWN;
    psoCI.GraphicsPipeline.PrimitiveTopology            = PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_VERTEX, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        // g_Particles 需要每帧更换（三缓冲轮转），必须使用 DYNAMIC 类型
        {SHADER_TYPE_VERTEX, "g_Particles", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pVS = vs;
    psoCI.pPS = ps;

    particlePSO_.Release();
    CreateGraphicsPSO(device_, psoCI, &particlePSO_);
    if (particlePSO_ == nullptr) {
        return false;
    }

    if (auto* varVS = particlePSO_->GetStaticVariableByName(SHADER_TYPE_VERTEX, "ParticleConstants");
        varVS != nullptr) {
        varVS->Set(particleConstants_);
    } else {
        return false;
    }
    if (auto* varPS = particlePSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "ParticleConstants"); varPS != nullptr) {
        varPS->Set(particleConstants_);
    } else {
        return false;
    }

    // g_Particles 是 DYNAMIC 变量，需要在每帧渲染前通过 SRB 设置，此处不绑定

    particleSRB_.Release();
    particlePSO_->CreateShaderResourceBinding(&particleSRB_, true);
    if (particleSRB_ != nullptr) {
        // 缓存热路径变量指针
        particleSRVVar_ = particleSRB_->GetVariableByName(SHADER_TYPE_VERTEX, "g_Particles");
    }
    return particleSRB_ != nullptr;
}

bool DiligentBackend::CreateParticleMeshShaderPSO() {
    // 检测 Mesh Shader 硬件支持
    if (!meshShadersChecked_) {
        meshShadersChecked_  = true;
        meshShaderSupported_ = false;
        useMeshShaders_      = false;

        if (device_ != nullptr) {
            const auto& features = device_->GetDeviceInfo().Features;

            // 记录设备信息以便调试
            MD3::DebugLog::Instance().Add(MD3::LogLevel::Info, "[GPU] Mesh Shader feature state: " +
                                                         std::to_string(static_cast<int>(features.MeshShaders)));

            // Mesh Shader 仅 D3D12 支持（Vulkan 需要额外扩展，暂不启用）
            if (backend_ == Backend::D3D12 && features.MeshShaders == DEVICE_FEATURE_STATE_ENABLED) {
                meshShaderSupported_ = true;
                useMeshShaders_      = true; // 默认启用
                MD3::DebugLog::Instance().Add(MD3::LogLevel::Info, "[GPU] Mesh Shaders supported and enabled");
            } else {
                std::string reason = (backend_ != Backend::D3D12) ? "not D3D12 backend" : "hardware not supported";
                MD3::DebugLog::Instance().Add(MD3::LogLevel::Info,
                                         "[GPU] Mesh Shaders not available (" + reason + "), using Vertex Pulling");
            }
        }
    }

    if (!useMeshShaders_) {
        return false; // 硬件不支持或已禁用，使用 Vertex Pulling 回退
    }

    if (device_ == nullptr || particleConstants_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    // Mesh Shader 仅 D3D12 支持，使用 DXIL 格式
    RefCntAutoPtr<IShader> ms = CreateShaderFromBytecode(
        device_, "SaturnParticle MS", SHADER_TYPE_MESH, SaturnParticleMesh_MS_DXIL, sizeof(SaturnParticleMesh_MS_DXIL));
    if (ms == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Warn, "[CreateParticleMeshShaderPSO] Mesh Shader creation failed");
        useMeshShaders_ = false;
        return false;
    }

    RefCntAutoPtr<IShader> ps =
        CreateShaderFromBytecode(device_, "SaturnParticle PS (Mesh)", SHADER_TYPE_PIXEL, SaturnParticleMesh_MeshPS_DXIL,
                                 sizeof(SaturnParticleMesh_MeshPS_DXIL));
    if (ps == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Warn, "[CreateParticleMeshShaderPSO] Pixel Shader creation failed");
        useMeshShaders_ = false;
        return false;
    }

    // 创建 Mesh PSO
    GraphicsPipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnParticle Mesh PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_MESH;

    psoCI.GraphicsPipeline.NumRenderTargets             = 1;
    psoCI.GraphicsPipeline.RTVFormats[0]                = kOffscreenColorFormat;
    psoCI.GraphicsPipeline.DSVFormat                    = TEX_FORMAT_UNKNOWN;
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

    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_MESH, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_PIXEL, "ParticleConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
        {SHADER_TYPE_MESH, "g_Particles", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pMS = ms;
    psoCI.pPS = ps;

    particleMeshPSO_.Release();
    device_->CreateGraphicsPipelineState(psoCI, &particleMeshPSO_);
    if (particleMeshPSO_ == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Warn, "[CreateParticleMeshShaderPSO] PSO creation failed");
        useMeshShaders_ = false;
        return false;
    }

    // 绑定常量缓冲
    if (auto* varMS = particleMeshPSO_->GetStaticVariableByName(SHADER_TYPE_MESH, "ParticleConstants");
        varMS != nullptr) {
        varMS->Set(particleConstants_);
    } else {
        useMeshShaders_ = false;
        return false;
    }
    if (auto* varPS = particleMeshPSO_->GetStaticVariableByName(SHADER_TYPE_PIXEL, "ParticleConstants");
        varPS != nullptr) {
        varPS->Set(particleConstants_);
    } else {
        useMeshShaders_ = false;
        return false;
    }

    particleMeshSRB_.Release();
    particleMeshPSO_->CreateShaderResourceBinding(&particleMeshSRB_, true);
    if (particleMeshSRB_ == nullptr) {
        useMeshShaders_ = false;
        return false;
    }

    MD3::DebugLog::Instance().Add(MD3::LogLevel::Info, "[GPU] Mesh Shader PSO created successfully");
    return true;
}

bool DiligentBackend::CreateParticleComputePSO() {
    if (device_ == nullptr || immediateContext_ == nullptr || particleComputeConstants_ == nullptr) {
        return false;
    }

    using namespace ShaderBytecodes;

    RefCntAutoPtr<IShader> cs;

    if (backend_ == Backend::Vulkan) {
        cs = CreateShaderFromBytecode(device_, "SaturnCompute CS", SHADER_TYPE_COMPUTE, SaturnCompute_CS_SPIRV,
                                      sizeof(SaturnCompute_CS_SPIRV));
    } else {
        // D3D11/D3D12 use DXBC
        cs = CreateShaderFromBytecode(device_, "SaturnCompute CS", SHADER_TYPE_COMPUTE, SaturnCompute_CS_DXBC,
                                      sizeof(SaturnCompute_CS_DXBC));
    }

    if (cs == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleComputePSO] Compute shader creation failed");
        return false;
    }

    ComputePipelineStateCreateInfo psoCI{};
    psoCI.PSODesc.Name         = "SaturnCompute PSO";
    psoCI.PSODesc.PipelineType = PIPELINE_TYPE_COMPUTE;

    // 变量声明顺序必须与 Vulkan GLSL 着色器中的 binding 索引一致：
    // - g_ParticlesIn: binding=0 (DYNAMIC - 需要每帧更换缓冲区)
    // - g_ParticlesOut: binding=1 (DYNAMIC - 需要每帧更换缓冲区)
    // - ComputeConstants: binding=2 (STATIC)
    // 注意：MUTABLE 只能在 SRB 创建后设置一次，DYNAMIC 才能每帧更新！
    const ShaderResourceVariableDesc vars[] = {
        {SHADER_TYPE_COMPUTE, "g_ParticlesIn", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "g_ParticlesOut", SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC},
        {SHADER_TYPE_COMPUTE, "ComputeConstants", SHADER_RESOURCE_VARIABLE_TYPE_STATIC},
    };
    psoCI.PSODesc.ResourceLayout.NumVariables = _countof(vars);
    psoCI.PSODesc.ResourceLayout.Variables    = vars;

    psoCI.pCS = cs;

    particleComputePSO_.Release();
    particleComputeSRB_.Release();
    CreateComputePSO(device_, psoCI, &particleComputePSO_);
    if (particleComputePSO_ == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleComputePSO] CreateComputePipelineState failed");
        return false;
    }

    if (auto* var = particleComputePSO_->GetStaticVariableByName(SHADER_TYPE_COMPUTE, "ComputeConstants");
        var != nullptr) {
        var->Set(particleComputeConstants_);
    } else {
        MD3::DebugLog::Instance().Add(
            MD3::LogLevel::Error, "[CreateParticleComputePSO] GetStaticVariableByName(ComputeConstants) returned nullptr");
        return false;
    }

    particleComputePSO_->CreateShaderResourceBinding(&particleComputeSRB_, true);
    if (particleComputeSRB_ == nullptr) {
        MD3::DebugLog::Instance().Add(MD3::LogLevel::Error, "[CreateParticleComputePSO] CreateShaderResourceBinding failed");
    } else {
        // 缓存热路径变量指针
        particleInVar_  = particleComputeSRB_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_ParticlesIn");
        particleOutVar_ = particleComputeSRB_->GetVariableByName(SHADER_TYPE_COMPUTE, "g_ParticlesOut");
    }
    return particleComputeSRB_ != nullptr;
}

void DiligentBackend::SimulateParticles(float dt, float handScale, float handHas) {
    if (immediateContext_ == nullptr || particleComputePSO_ == nullptr || particleComputeSRB_ == nullptr ||
        particleComputeConstants_ == nullptr) {
        MD3::DebugLog::Instance().AddOnce("SimulateParticles_Null", MD3::LogLevel::Warn,
                                     "[SimulateParticles] skipped: compute pipeline not ready");
        return;
    }
    if (particleCount_ == 0) {
        return;
    }
    // 复刻 OpenGL 的三缓冲索引用法（见 OpenGL: DoubleBufferSSBO::Swap）：
    // - readIdx：本帧计算着色器的输入
    // - writeIdx：本帧计算着色器的输出
    // - renderIdx：本帧渲染使用的数据（Swap 后等于上一帧的 readIdx）
    if (particleSRVs_[particleReadIdx_] == nullptr || particleUAVs_[particleWriteIdx_] == nullptr) {
        MD3::DebugLog::Instance().AddOnce("SimulateParticles_SrvUavNull", MD3::LogLevel::Warn,
                                     "[SimulateParticles] skipped: SRV/UAV is null");
        return;
    }

    // 防止 dt 为 0 导致粒子静止（尤其是第一帧）
    if (dt < 0.001f) {
        dt = 0.016f;
    }

    // 更新 Compute 常量（1:1 对齐 OpenGL uniform：uDt/uHandScale/uHandHas/uParticleCount）
    {
        PVoid mapped = nullptr;
        immediateContext_->MapBuffer(particleComputeConstants_, MAP_WRITE, MAP_FLAG_DISCARD, mapped);
        if (mapped != nullptr) {
            auto* cb          = static_cast<ParticleComputeConstants*>(mapped);
            cb->Dt            = dt;
            cb->HandScale     = handScale;
            cb->HandHas       = handHas;
            cb->ParticleCount = particleCount_;
            immediateContext_->UnmapBuffer(particleComputeConstants_, MAP_WRITE);
        }
    }

    // 使用缓存的变量指针设置 compute shader 资源（避免每帧字符串查找）
    if (particleInVar_ != nullptr) {
        particleInVar_->Set(particleSRVs_[particleReadIdx_]);
    } else {
        return;
    }

    if (particleOutVar_ != nullptr) {
        particleOutVar_->Set(particleUAVs_[particleWriteIdx_]);
    } else {
        return;
    }

    immediateContext_->SetPipelineState(particleComputePSO_);
    immediateContext_->CommitShaderResources(particleComputeSRB_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    DispatchComputeAttribs dispatch{};
    dispatch.ThreadGroupCountX = (particleCount_ + 255u) / 256u;
    dispatch.ThreadGroupCountY = 1;
    dispatch.ThreadGroupCountZ = 1;
    immediateContext_->DispatchCompute(dispatch);

    // 显式的资源状态转换：确保 UAV 写入对后续的 SRV 读取可见
    // 这是等价于 OpenGL 的 glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT)
    // 注意：使用 RESOURCE_STATE_UNKNOWN 让 Diligent 自动检测当前状态，避免第一帧时状态不匹配
    {
        StateTransitionDesc barrier{};
        barrier.pResource      = particleBuffers_[particleWriteIdx_];
        barrier.OldState       = RESOURCE_STATE_UNKNOWN;
        barrier.NewState       = RESOURCE_STATE_SHADER_RESOURCE;
        barrier.TransitionType = STATE_TRANSITION_TYPE_IMMEDIATE;
        barrier.Flags          = STATE_TRANSITION_FLAG_UPDATE_STATE;
        immediateContext_->TransitionResourceStates(1, &barrier);
    }

    // 三缓冲轮转（完全复刻 OpenGL 版本 DoubleBufferSSBO::Swap）：
    // - renderIdx <- readIdx：上一帧计算完成的数据变为渲染数据
    // - readIdx <- writeIdx：本帧写入的变为下一帧读取（计算输入）
    // - writeIdx <- oldRender：渲染完的缓冲变为下一帧写入目标
    //
    // 这意味着渲染总是落后计算一帧，这是正常的三缓冲流水线行为。
    // 从第二帧开始，渲染的数据就是经过计算更新的。
    const uint32_t oldRender = particleRenderIdx_;
    particleRenderIdx_       = particleReadIdx_;
    particleReadIdx_         = particleWriteIdx_;
    particleWriteIdx_        = oldRender;

    // g_Particles 是 DYNAMIC 变量，将在 RenderOffscreen 的 CommitShaderResources 前通过 SRB 设置
}

} // namespace ParticleSaturn::Render
