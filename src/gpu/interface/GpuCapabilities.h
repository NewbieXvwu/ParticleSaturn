#pragma once

namespace ParticleSaturn::Gpu {

struct GpuCapabilities {
    bool supportsCompute            = false;
    bool supportsStorageBuffer      = false;
    bool supportsIndirectDraw       = false;
    bool supportsMeshShader         = false;
    bool supportsTransparentSurface = false;
    bool supportsTimestamp          = false;
    bool supportsProgramCache       = false;
    bool supportsAdaptiveVSync      = false;
};

} // namespace ParticleSaturn::Gpu
