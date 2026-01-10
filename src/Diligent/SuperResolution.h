#pragma once

#include <cstdint>

namespace ParticleSaturn::Render {

// 预留：未来接入 DLSS/FSR/XeSS 等超分时使用。
// 目前不实现任何具体算法，只提供接口形状，避免后续大范围改动渲染主循环。
struct SuperResolutionInputs {
    // 输出分辨率（窗口/SwapChain 分辨率）
    uint32_t OutputWidth  = 0;
    uint32_t OutputHeight = 0;

    // 渲染分辨率（内部渲染 RT 分辨率）
    uint32_t RenderWidth  = 0;
    uint32_t RenderHeight = 0;
};

class ISuperResolution {
  public:
    virtual ~ISuperResolution() = default;

    // 当输出/渲染分辨率变化时重建内部资源。
    virtual void OnResize(const SuperResolutionInputs& inputs) = 0;
};

} // namespace ParticleSaturn::Render
