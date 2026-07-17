#include "ParticleAbi.h"

#include <cassert>
#include <cstddef>
#include <fstream>
#include <string>

using ParticleSaturn::ShaderAbi::Particle;

static_assert(sizeof(Particle) == 32);
static_assert(offsetof(Particle, position) == 0);
static_assert(offsetof(Particle, color) == 16);
static_assert(offsetof(Particle, speed) == 20);
static_assert(offsetof(Particle, isRing) == 24);
static_assert(offsetof(Particle, padding) == 28);

int main() {
    std::ifstream input{PARTICLESATURN_PARTICLE_ABI_MSL_PATH};
    assert(input);
    const std::string source{std::istreambuf_iterator<char>{input}, {}};
    assert(source.find("struct Particle") != std::string::npos);
    assert(source.find("float4 position") != std::string::npos);
    assert(source.find("uint color") != std::string::npos);
    assert(source.find("float speed") != std::string::npos);
    assert(source.find("uint isRing") != std::string::npos);
    assert(source.find("uint padding") != std::string::npos);
    return 0;
}
