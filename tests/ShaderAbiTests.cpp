#include "ParticleAbi.h"

#include <cstddef>

using ParticleSaturn::ShaderAbi::Particle;

static_assert(sizeof(Particle) == 32);
static_assert(offsetof(Particle, position) == 0);
static_assert(offsetof(Particle, color) == 16);
static_assert(offsetof(Particle, speed) == 20);
static_assert(offsetof(Particle, isRing) == 24);
static_assert(offsetof(Particle, padding) == 28);

int main() { return 0; }
