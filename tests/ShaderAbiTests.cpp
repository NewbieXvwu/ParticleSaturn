#include "ParticleAbi.h"

#include <cassert>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

using ParticleSaturn::ShaderAbi::Particle;

static_assert(sizeof(Particle) == 32);
static_assert(offsetof(Particle, position) == 0);
static_assert(offsetof(Particle, color) == 16);
static_assert(offsetof(Particle, speed) == 20);
static_assert(offsetof(Particle, isRing) == 24);
static_assert(offsetof(Particle, padding) == 28);

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input{path};
    assert(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}

void AssertParticleLayout(const std::string& source, const char* float4) {
    assert(source.find("struct Particle") != std::string::npos);
    assert(source.find(std::string{float4} + " position") != std::string::npos);
    assert(source.find("uint color") != std::string::npos);
    assert(source.find("float speed") != std::string::npos);
    assert(source.find("uint isRing") != std::string::npos);
    assert(source.find("uint padding") != std::string::npos);
}

} // namespace

int main() {
    AssertParticleLayout(ReadFile(PARTICLESATURN_PARTICLE_ABI_HLSL_PATH), "float4");
    AssertParticleLayout(ReadFile(PARTICLESATURN_PARTICLE_ABI_GLSL_PATH), "vec4");
    AssertParticleLayout(ReadFile(PARTICLESATURN_PARTICLE_ABI_MSL_PATH), "float4");

    const std::filesystem::path sourceRoot{PARTICLESATURN_SOURCE_DIRECTORY};
    const std::array shaderPaths{
        sourceRoot / "src/shaders/hlsl/SaturnParticle_VS.hlsl",
        sourceRoot / "src/shaders/hlsl/SaturnCompute_CS.hlsl",
        sourceRoot / "src/shaders/hlsl/SaturnInit_CS.hlsl",
        sourceRoot / "src/shaders/hlsl/SaturnParticleMesh_MS.hlsl",
        sourceRoot / "src/shaders/glsl/SaturnParticle_VS.glsl",
        sourceRoot / "src/shaders/glsl/SaturnCompute_CS.glsl",
        sourceRoot / "src/shaders/glsl/SaturnInit_CS.glsl",
        sourceRoot / "src/shaders/msl/ParticleKernels.metal",
    };
    for (const auto& path : shaderPaths) {
        const auto source = ReadFile(path);
        assert(source.find("Particle.") != std::string::npos);
        assert(source.find("struct ParticleData") == std::string::npos);
    }
    return 0;
}
