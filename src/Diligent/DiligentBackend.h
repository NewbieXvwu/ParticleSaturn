#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "../AppState.h"
#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderBackend.h"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "SwapChain.h"
#include "Texture.h"

struct HWND__;
using HWND = HWND__*;

namespace ParticleSaturn::UI {
class ImGuiDiligent;
}

namespace ParticleSaturn::Render {

class DiligentBackend final {
  public:
    DiligentBackend();
    ~DiligentBackend();

    DiligentBackend(const DiligentBackend&)            = delete;
    DiligentBackend& operator=(const DiligentBackend&) = delete;
    DiligentBackend(DiligentBackend&&)                 = delete;
    DiligentBackend& operator=(DiligentBackend&&)      = delete;

    bool Init(Backend backend, HWND hwnd, SurfaceSize initialSize, AppState* state);
    void Shutdown();

    void Resize(SurfaceSize newSize);
    void RenderFrame();

    // 处理 Win32 消息（ImGui 输入）
    bool HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam);

    Backend GetBackend() const { return backend_; }

    bool IsInitialized() const { return swapChain_ != nullptr; }

    const std::wstring& GetLastError() const { return lastError_; }

    AppState* GetAppState() const { return appState_; }

  private:
    void SetLastError(const wchar_t* msg) { lastError_ = (msg != nullptr) ? msg : L""; }

    bool CreateFullscreenQuadPSO();
    bool CreateOffscreenRenderTarget(SurfaceSize size);
    bool CreateStarfieldPSO();
    bool CreateStarfieldBuffers(uint32_t starCount);
    bool CreateParticleBuffers(uint32_t maxParticles);
    bool CreateParticlePSO();
    bool CreateParticleComputePSO();
    bool CreateSevenSegmentPSO();
    bool CreateSevenSegmentBuffers();

    // Bloom / Blur (用于 bloom 合成与 UI 玻璃模糊的基础纹理)
    bool CreateBloomPSO();
    bool CreateBloomTextures(SurfaceSize size);
    void RenderBloom();

    void RenderOffscreen();
    void BlitOffscreenToBackBuffer();
    void RenderClear();
    void UpdateFullscreenQuadBindings();
    void SimulateParticles(float dt, float handScale, float handHas);
    void RenderSevenSegmentFPS();

    Backend      backend_ = Backend::D3D12;
    SurfaceSize  surfaceSize_{};
    std::wstring lastError_;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice>  device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain>     swapChain_;

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         fullscreenQuadPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> fullscreenQuadSRB_;

    std::chrono::steady_clock::time_point startTime_    = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnimTime_ = std::chrono::steady_clock::time_point{};
    float                                 animAutoTime_ = 0.0f;
    float                                 animScale_    = 1.0f;
    float                                 animRotX_     = 0.4f;
    float                                 animRotY_     = 0.0f;

    // FPS 计算（简单移动平均）
    static constexpr int                  kFpsSampleCount = 60;
    float                                 fpsSamples_[kFpsSampleCount]{};
    int                                   fpsSampleIndex_ = 0;
    float                                 currentFps_     = 60.0f;
    std::chrono::steady_clock::time_point lastFrameTime_{};

    // 动态 LOD（对齐 OpenGL：每 0.5s 检查一次，自动调节粒子数与 pixelRatio）
    float    lodUpdateTimer_       = 0.0f;
    uint32_t lastLodParticleCount_ = 0;
    float    lastLodPixelRatio_    = 0.0f;
    bool     lastLodBasisValid_    = false;
    int      totalFrameCount_      = 0;

    // FPS 历史曲线（低频采样）
    static constexpr int   kFpsHistorySize = 60;
    float                  fpsHistory_[kFpsHistorySize]{};
    int                    fpsHistoryIndex_          = 0;
    float                  fpsHistorySampleTimer_    = 0.0f;
    static constexpr float kFpsHistorySampleInterval = 0.05f; // 50ms 采样一次 (与 OpenGL 版一致)

    // FPS 曲线动画
    float fpsGraphAnimMinVal_     = 0.0f;   // Y 轴最小值动画
    float fpsGraphAnimMaxVal_     = 120.0f; // Y 轴最大值动画
    float fpsGraphScrollAnimTime_ = 0.0f;   // 滚动动画累计时间（用于平滑滚动）
    bool  fpsGraphFirstFrame_     = true;   // 首帧标记

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         starPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> starSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                starVB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                starConstants_;
    uint32_t                                                  starCount_ = 0;

    static constexpr uint32_t kParticleBufferCount = 3;
    uint32_t                  particleRenderIdx_   = 0;
    uint32_t                  particleReadIdx_     = 0;
    uint32_t                  particleWriteIdx_    = 1;
    uint32_t                  particleCount_       = 0;

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         particlePSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> particleSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                particleConstants_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                particleIndirectArgs_;

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         particleComputePSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> particleComputeSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                particleComputeConstants_;

    Diligent::RefCntAutoPtr<Diligent::IBuffer>     particleBuffers_[kParticleBufferCount];
    Diligent::RefCntAutoPtr<Diligent::IBufferView> particleSRVs_[kParticleBufferCount];
    Diligent::RefCntAutoPtr<Diligent::IBufferView> particleUAVs_[kParticleBufferCount];

    Diligent::RefCntAutoPtr<Diligent::ITexture>     offscreenColor_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> offscreenRTV_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> offscreenSRV_;

    // Bloom constants for fullscreen quad
    Diligent::RefCntAutoPtr<Diligent::IBuffer> bloomConstants_;
    float                                      bloomStrength_ = 0.5f;

    // Bloom blur pipeline（低分辨率 Kawase blur）
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         bloomDownsamplePSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> bloomDownsampleSRB_;

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         bloomBlurPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> bloomBlurSRB_;

    Diligent::RefCntAutoPtr<Diligent::IBuffer> bloomBlurConstants_;

    Diligent::RefCntAutoPtr<Diligent::ITexture>     bloomTexA_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomRTV_A_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomSRV_A_;

    Diligent::RefCntAutoPtr<Diligent::ITexture>     bloomTexB_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomRTV_B_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomSRV_B_;

    uint32_t bloomW_ = 0;
    uint32_t bloomH_ = 0;

    // 七段数码管 FPS 显示
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         sevenSegPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> sevenSegSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                sevenSegConstants_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                sevenSegVB_[10]; // 每个数字一个 VB
    uint32_t                                                  sevenSegVertexCount_[10]{};

    // ImGui integration
    std::unique_ptr<UI::ImGuiDiligent> imgui_;
    HWND                               hwnd_ = nullptr;

    // 全局应用状态（由外部传入，生命周期由调用方管理）
    AppState* appState_ = nullptr;
};

} // namespace ParticleSaturn::Render
