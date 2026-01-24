#pragma once

#include "RenderBackend.h"
#include "Shader.h"

namespace ParticleSaturn::Render {

struct ShaderSources {
    const char*                      Vertex   = nullptr;
    const char*                      Fragment = nullptr;
    Diligent::SHADER_SOURCE_LANGUAGE Language = Diligent::SHADER_SOURCE_LANGUAGE_DEFAULT;
};

struct ComputeShaderSource {
    const char*                      Source   = nullptr;
    Diligent::SHADER_SOURCE_LANGUAGE Language = Diligent::SHADER_SOURCE_LANGUAGE_DEFAULT;
};

ShaderSources       GetFullscreenQuadShaderSources(Backend backend);
ShaderSources       GetBloomDownsampleShaderSources(Backend backend);
ShaderSources       GetBloomBlurShaderSources(Backend backend);
ShaderSources       GetAcrylicCompositeShaderSources(Backend backend);
ComputeShaderSource GetSaturnComputeShaderSource(Backend backend);
ComputeShaderSource GetSaturnInitComputeShaderSource(Backend backend); // GPU 粒子初始化

// Mesh Shader 粒子渲染（D3D12/Vulkan 专用）
struct MeshShaderSources {
    const char*                      Mesh     = nullptr;
    const char*                      Fragment = nullptr;
    Diligent::SHADER_SOURCE_LANGUAGE Language = Diligent::SHADER_SOURCE_LANGUAGE_DEFAULT;
};

MeshShaderSources GetSaturnParticleMeshShaderSources(Backend backend);

ShaderSources GetStarShaderSources(Backend backend);
ShaderSources GetSaturnParticleShaderSources(Backend backend);

} // namespace ParticleSaturn::Render
