#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "../AppState.h"
#include "Buffer.h"
#include "BufferView.h"
#include "DeviceContext.h"
#include "DirectCompositionSwapChain.h"
#include "PipelineState.h"
#include "RefCntAutoPtr.hpp"
#include "RenderBackend.h"
#include "RenderDevice.h"
#include "ShaderResourceBinding.h"
#include "SwapChain.h"
#include "Texture.h"
#include "VulkanD3D12Interop.h"

struct HWND__;
using HWND = HWND__*;

namespace ParticleSaturn::UI {
class ImGuiDiligent;
}

namespace ParticleSaturn::HandTracking {
class Controller;
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
    void RequestResize(SurfaceSize newSize);
    void RenderFrame();

    // 处理 Win32 消息（ImGui 输入）
    bool HandleWin32Message(HWND hwnd, unsigned int msg, unsigned long long wParam, long long lParam);

    // 应用 DWM Backdrop 模式，并在需要时切换 SwapChain 透明模式（D3D11/D3D12）。
    // mode: 0=Solid, 1=Aero, 2=Acrylic, 3=Mica（与 Win32WindowManager::BackdropName 一致）
    bool SetBackdropMode(int mode);

    Backend GetBackend() const { return backend_; }

    bool IsInitialized() const {
        return swapChain_ != nullptr || dcompSwapChain_.IsInitialized() ||
               (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized());
    }

    const std::wstring& GetLastError() const { return lastError_; }

    AppState* GetAppState() const { return appState_; }

  private:
    void SetLastError(const wchar_t* msg) { lastError_ = (msg != nullptr) ? msg : L""; }

    bool CreateFullscreenQuadPSO();
    bool CreateOffscreenRenderTarget(SurfaceSize size);
    bool CreateStarfieldPSO();
    bool CreateStarfieldBuffers(uint32_t starCount);
    bool CreateParticleBuffers(uint32_t maxParticles);
    bool CreateParticleBuffersGPU(uint32_t maxParticles); // GPU 初始化版本
    bool CreateParticleInitPSO();                          // GPU 初始化 PSO
    bool CreateParticlePSO();
    bool CreateParticleMeshShaderPSO();                    // Mesh Shader 路径
    bool CreateParticleComputePSO();
    bool CreateSevenSegmentPSO();
    bool CreateSevenSegmentBuffers();

    // Bloom / Blur (用于 bloom 合成与 UI 玻璃模糊的基础纹理)
    bool CreateBloomPSO();
    bool CreateBloomTextures(SurfaceSize size);
    void RenderBloom();

    // UI Blur：把“最终显示的场景颜色”（offscreen + bloom + tone mapping）解析到中间纹理后再做低分辨率模糊，
    // 供 ImGui/MD3 采样，避免误用 Bloom 的 bright-pass 结果。
    bool CreateUISceneTextures(SurfaceSize size);
    void RenderUISceneForUI();
    void RenderUIBlur();
    bool CreateAcrylicPSO();
    void RenderAcrylicComposite();

    void RenderOffscreen();
    void BlitOffscreenToBackBuffer();
    void RenderClear();
    void UpdateFullscreenQuadBindings();
    void SimulateParticles(float dt, float handScale, float handHas);
    void RenderSevenSegmentFPS();

    // Debug Log Panel icons (pause/resume)
    Diligent::ITextureView* GetOrCreateLogControlIconSRV(bool pausedState /* true=resume icon, false=pause icon */);

    // DirectComposition SwapChain 辅助方法
    Diligent::ITextureView* GetCurrentBackBufferRTV();
    bool                    CreateDCompBackBufferRTVs();
    bool UpdateD3D11CurrentBackBufferRTV(); // D3D11 每帧更新当前后缓冲 RTV
    bool InitD3D11NativeBlit();             // 初始化 D3D11 原生 blit 管线
    void BlitOffscreenToBackBufferD3D11();  // D3D11 透明模式专用的原生 blit
    void                    PresentFrame(int syncInterval);

    // 运行时切换透明模式（D3D11/D3D12）
    // @param enableTransparent true=启用透明（DComp SwapChain），false=禁用（标准 SwapChain）
    // @return 是否成功
    bool SwitchTransparentMode(bool enableTransparent);

    Backend      backend_ = Backend::D3D12;
    SurfaceSize  surfaceSize_{};
    std::wstring lastError_;

    Diligent::RefCntAutoPtr<Diligent::IRenderDevice>  device_;
    Diligent::RefCntAutoPtr<Diligent::IDeviceContext> immediateContext_;
    Diligent::RefCntAutoPtr<Diligent::ISwapChain>     swapChain_;

    // DirectComposition SwapChain（D3D12 透明模式专用）
    DirectCompositionSwapChain                      dcompSwapChain_;
    bool                                            useDCompSwapChain_ = false;
    static constexpr uint32_t                       kDCompBufferCount  = 3;
    Diligent::RefCntAutoPtr<Diligent::ITexture>     dcompBackBuffers_[kDCompBufferCount];
    Diligent::RefCntAutoPtr<Diligent::ITextureView> dcompBackBufferRTVs_[kDCompBufferCount];

    // ============================================================================
    // D3D11 透明模式优化：使用原生 D3D11 API 避免每帧 Diligent 纹理包装开销
    // 包括：原生着色器、状态对象、采样器、RTV 等
    // ============================================================================
    bool d3d11NativeBlitInitialized_ = false;
    // 缓存上一帧的 D3D11 后缓冲指针，用于检测资源是否变化
    void* d3d11LastBackBufferPtr_ = nullptr;

    // 原生 D3D11 资源（仅在 D3D11 透明模式下使用）
    Microsoft::WRL::ComPtr<ID3D11Device>             d3d11Device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>      d3d11Context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>       d3d11BlitVS_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>        d3d11BlitPS_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       d3d11LinearSampler_;
    Microsoft::WRL::ComPtr<ID3D11BlendState>         d3d11BlendState_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>    d3d11RasterizerState_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState>  d3d11DepthStencilState_;
    Microsoft::WRL::ComPtr<ID3D11Buffer>             d3d11BloomCB_;         // 常量缓冲（bloom strength 等）
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView>   d3d11CachedRTV_;       // 缓存后缓冲 RTV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> d3d11OffscreenSRV_;    // offscreen 纹理的原生 SRV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> d3d11BloomSRV_;        // bloom 纹理的原生 SRV
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> d3d11UISceneSRV_;      // UI scene 纹理的原生 SRV

    // Vulkan D3D12 互操作层（Vulkan 透明模式专用）
    std::unique_ptr<VulkanD3D12Interop> vkD3D12Interop_;
    bool                                useVkD3D12Interop_ = false;

    // 延迟 resize：避免在 WndProc 里做重资源操作导致卡顿/假死
    SurfaceSize pendingResize_{};
    bool        hasPendingResize_ = false;

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         fullscreenQuadPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> fullscreenQuadSRB_;

    // ============================================================================
    // 热路径变量指针缓存（避免每帧 GetVariableByName 字符串查找）
    // 在 PSO/SRB 创建成功后初始化，PSO 重建时更新
    // ============================================================================
    Diligent::IShaderResourceVariable* fullscreenTexVar_   = nullptr; // g_Texture
    Diligent::IShaderResourceVariable* fullscreenBloomVar_ = nullptr; // g_BloomTexture
    Diligent::IShaderResourceVariable* bloomDownTexVar_    = nullptr; // g_Texture (downsample)
    Diligent::IShaderResourceVariable* bloomBlurTexVar_    = nullptr; // g_Texture (blur)
    Diligent::IShaderResourceVariable* acrylicTexVar_      = nullptr; // g_Texture
    Diligent::IShaderResourceVariable* particleInVar_      = nullptr; // g_ParticlesIn (compute)
    Diligent::IShaderResourceVariable* particleOutVar_     = nullptr; // g_ParticlesOut (compute)
    Diligent::IShaderResourceVariable* particleSRVVar_     = nullptr; // g_Particles (vertex)

    std::chrono::steady_clock::time_point startTime_    = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastAnimTime_ = std::chrono::steady_clock::time_point{};
    float                                 animAutoTime_ = 0.0f;
    float                                 animScale_    = 1.0f;
    float                                 animRotX_     = 0.4f;
    float                                 animRotY_     = 0.0f;

    // FPS 计算（帧时间调和平均）
    static constexpr int                  kFpsSampleCount = 60;
    float                                 frameDtSamples_[kFpsSampleCount]{};
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

    // FPS 曲线增量更新缓存
    float fpsHistoryCachedMin_     = 0.0f;   // 缓存的最小值
    float fpsHistoryCachedMax_     = 120.0f; // 缓存的最大值
    bool  fpsHistoryCacheDirty_    = true;   // 缓存是否需要重新计算
    int   fpsHistoryValidCount_    = 0;      // 有效数据点数量

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

    // Mesh Shader 路径（硬件支持时使用，否则回退到 Vertex Pulling）
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         particleMeshPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> particleMeshSRB_;
    bool                                                      meshShaderSupported_ = false; // 硬件是否支持 Mesh Shader
    bool                                                      useMeshShaders_      = false; // 实际是否启用（可调试切换）
    bool                                                      meshShadersChecked_  = false; // 是否已检测

    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         particleComputePSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> particleComputeSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                particleComputeConstants_;

    // GPU 粒子初始化（一次性执行，替代 CPU 初始化）
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         particleInitPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> particleInitSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                particleInitConstants_;
    bool                                                      useGPUParticleInit_ = false; // TODO: GPU init 有问题，暂时禁用

    Diligent::RefCntAutoPtr<Diligent::IBuffer>     particleBuffers_[kParticleBufferCount];
    Diligent::RefCntAutoPtr<Diligent::IBufferView> particleSRVs_[kParticleBufferCount];
    Diligent::RefCntAutoPtr<Diligent::IBufferView> particleUAVs_[kParticleBufferCount];

    Diligent::RefCntAutoPtr<Diligent::ITexture>     offscreenColor_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> offscreenRTV_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> offscreenSRV_;

    // Bloom constants for fullscreen quad
    Diligent::RefCntAutoPtr<Diligent::IBuffer> bloomConstants_;
    float                                      bloomStrength_             = 0.5f;
    float                                      bloomStrengthBeforeTransp_ = 0.5f; // 开启透明前的辉光值
    bool                                       bloomEnabled_              = true; // Bloom 开关（默认启用）

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

    // 次级模糊纹理 (1/12 分辨率，用于折叠区域 Acrylic 效果)
    Diligent::RefCntAutoPtr<Diligent::ITexture>     bloomTexC_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomRTV_C_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomSRV_C_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>     bloomTexD_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomRTV_D_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> bloomSRV_D_;

    uint32_t bloomW_  = 0;
    uint32_t bloomH_  = 0;
    uint32_t bloomW2_ = 0; // 1/12 分辨率宽度
    uint32_t bloomH2_ = 0; // 1/12 分辨率高度

    // UI 场景解析纹理（与 SwapChain 颜色格式一致）
    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiSceneColor_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiSceneRTV_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiSceneSRV_;

    // UI 模糊纹理（1/6 强模糊，1/12 弱模糊）
    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiBlurTexA_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurRTV_A_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurSRV_A_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiBlurTexB_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurRTV_B_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurSRV_B_;

    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiBlurTexC_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurRTV_C_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurSRV_C_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiBlurTexD_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurRTV_D_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiBlurSRV_D_;

    uint32_t uiBlurW_  = 0;
    uint32_t uiBlurH_  = 0;
    uint32_t uiBlurW2_ = 0;
    uint32_t uiBlurH2_ = 0;

    // Acrylic 合成（在低分辨率模糊纹理上进行：饱和度增强 + 近似 exclusion + tint 调制）
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         acrylicPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> acrylicSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                acrylicConstants_;

    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiAcrylicStrong_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiAcrylicRTV_Strong_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiAcrylicSRV_Strong_;

    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiAcrylicWeak_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiAcrylicRTV_Weak_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiAcrylicSRV_Weak_;

    // 噪点纹理（全分辨率，避免依赖 wrap sampler）
    Diligent::RefCntAutoPtr<Diligent::ITexture>     uiNoiseTex_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> uiNoiseSRV_;

    // Debug Log 控制图标（ImGui::Image）
    Diligent::RefCntAutoPtr<Diligent::ITexture>     logPauseIconTex_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> logPauseIconSRV_;
    Diligent::RefCntAutoPtr<Diligent::ITexture>     logResumeIconTex_;
    Diligent::RefCntAutoPtr<Diligent::ITextureView> logResumeIconSRV_;

    // 七段数码管 FPS 显示
    Diligent::RefCntAutoPtr<Diligent::IPipelineState>         sevenSegPSO_;
    Diligent::RefCntAutoPtr<Diligent::IShaderResourceBinding> sevenSegSRB_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                sevenSegConstants_;
    Diligent::RefCntAutoPtr<Diligent::IBuffer>                sevenSegVB_[10]; // 每个数字一个 VB
    uint32_t                                                  sevenSegVertexCount_[10]{};

    // ImGui integration
    std::unique_ptr<UI::ImGuiDiligent> imgui_;
    HWND                               hwnd_ = nullptr;

    // Hand tracking (MediaPipe/TFLite via HandTracker library)
    std::unique_ptr<HandTracking::Controller> handTracker_;

    // 全局应用状态（由外部传入，生命周期由调用方管理）
    AppState* appState_ = nullptr;
};

} // namespace ParticleSaturn::Render
