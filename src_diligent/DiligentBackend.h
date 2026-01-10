#pragma once

#include "RenderBackend.h"

#include "DeviceContext.h"
#include "RefCntAutoPtr.hpp"
#include "RenderDevice.h"
#include "SwapChain.h"

#include <cstdint>

struct HWND__;
using HWND = HWND__*;

namespace ParticleSaturn::Render {

class DiligentBackend final {
  public:
    DiligentBackend() = default;
    ~DiligentBackend() = default;

    DiligentBackend(const DiligentBackend&)            = delete;
    DiligentBackend& operator=(const DiligentBackend&) = delete;
    DiligentBackend(DiligentBackend&&)                 = delete;
    DiligentBackend& operator=(DiligentBackend&&)      = delete;

    bool Init(Backend backend, HWND hwnd, SurfaceSize initialSize);
    void Shutdown();

    void Resize(SurfaceSize newSize);
    void RenderFrame();

    Backend GetBackend() const { return backend_; }
    bool    IsInitialized() const { return swapChain_ != nullptr; }

  private:
    void RenderClear();

    Backend backend_ = Backend::D3D12;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice>  device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain>     swapChain_;
};

} // namespace ParticleSaturn::Render
