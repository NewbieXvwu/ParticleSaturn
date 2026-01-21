#pragma once

#include "GraphicsTypes.h"
#include "RenderBackend.h"

namespace ParticleSaturn::Render {

struct ShaderSources {
    const char*                      Vertex   = nullptr;
    const char*                      Fragment = nullptr;
    Diligent::SHADER_SOURCE_LANGUAGE  Language = Diligent::SHADER_SOURCE_LANGUAGE_DEFAULT;
};

struct ComputeShaderSource {
    const char*                      Source   = nullptr;
    Diligent::SHADER_SOURCE_LANGUAGE  Language = Diligent::SHADER_SOURCE_LANGUAGE_DEFAULT;
};

ShaderSources       GetFullscreenQuadShaderSources(Backend backend);
ShaderSources       GetBloomDownsampleShaderSources(Backend backend);
ShaderSources       GetBloomBlurShaderSources(Backend backend);
ShaderSources       GetAcrylicCompositeShaderSources(Backend backend);
ComputeShaderSource GetSaturnComputeShaderSource(Backend backend);
ShaderSources       GetStarShaderSources(Backend backend);
ShaderSources       GetSaturnParticleShaderSources(Backend backend);

} // namespace ParticleSaturn::Render
