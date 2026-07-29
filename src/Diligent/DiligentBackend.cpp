#include "DiligentBackend.h"
#include "DiligentBackendInternal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <random>
#include <thread>
#include <vector>

#include "../DebugLog.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "../Settings.h"
#include "../ShaderCompileProgress.h"
#include "../generated/LogControlIcons.h"
#include "ShaderBytecodes.h"  // 构建期生成于 ${CMAKE_BINARY_DIR}/generated，经 include 目录解析

#include "ArchiverFactoryLoader.h"
#include "CommandQueueD3D12.h"
#include "CrashAnalyzer.h"
#include "DataBlobImpl.hpp"
#include "DeviceContextD3D12.h"
#include "DiligentShaderSources.h"
#include "EngineFactoryD3D11.h"
#include "EngineFactoryD3D12.h"
#include "EngineFactoryVk.h"
#include "GraphicsTypes.h"
#include "HandTracker.h"
#include "ImGuiDiligent.h"
#include "InputLayout.h"
#include "NativeWindow.h"
#include "RenderDeviceD3D11.h"
#include "RenderDeviceD3D12.h"
#include "Sampler.h"
#include "TextureViewD3D11.h" // For ITextureViewD3D11 in native D3D11 blit
#include "VulkanD3D12Interop.h"
#include "platform/windows/Win32WindowManager.h"
#include "imgui.h"
#include "md3/MD3.h"
#include "md3/MD3Log.h"  // D-015 Phase B：后端日志改写入 MD3::DebugLog，供共享面板 Log 区展示

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d12.h>
#include <d3dcompiler.h>            // D3DCompile for native D3D11 blit shaders
#pragma comment(lib, "d3dcompiler") // Link with d3dcompiler.lib for D3DCompile
#include <dwmapi.h>
#include <wrl/client.h> // For Microsoft::WRL::ComPtr

namespace ParticleSaturn::Render {

using namespace Diligent;

// 构造函数和析构函数
DiligentBackend::DiligentBackend() = default;

DiligentBackend::~DiligentBackend() {
    Shutdown();
}

// D-015 后续（cc8e4a）：原匿名命名空间的结构体/数学/着色器与 PSO 辅助、粒子初始化
// 已上提到 DiligentBackendInternal.h（detail 命名空间），供拆分后的多个 TU 共享。
using namespace detail;

bool DiligentBackend::Init(Backend backend, HWND hwnd, SurfaceSize initialSize, AppState* state) {
    backend_  = backend;
    appState_ = state;
    SetLastError(nullptr);

    if (hwnd == nullptr || initialSize.Width == 0 || initialSize.Height == 0) {
        SetLastError(L"Init 参数无效（HWND 或尺寸为 0）。");
        return false;
    }

    // VSync 行为：
    // - OpenGL：若支持 Adaptive（-1），默认启用；否则回退为 On（1）。
    // - D3D12：已知 -1 在某些环境可能导致启动即白屏/卡死，故标记为不支持并强制回退到 1。
    // - Vulkan：支持自适应 VSync。Diligent 在 SyncInterval=1 时优先选择 FIFO_RELAXED（自适应），
    //   帧来得及则等 VBlank，帧晚则立即显示（可能撕裂），实现低延迟的帧率限制。
    if (appState_ != nullptr) {
        appState_->render.adaptiveVSyncSupported = (backend_ == Backend::Vulkan);
        if (appState_->render.vsyncMode < 0 && !appState_->render.adaptiveVSyncSupported) {
            appState_->render.vsyncMode = 1;
        }
    }

    device_.Release();
    immediateContext_.Release();
    swapChain_.Release();

    const NativeWindow window{reinterpret_cast<void*>(hwnd)};

    SwapChainDesc scDesc{};
    scDesc.Width             = initialSize.Width;
    scDesc.Height            = initialSize.Height;
    scDesc.ColorBufferFormat = TEX_FORMAT_RGBA8_UNORM_SRGB;
    // SwapChain 缓冲数固定为 3：
    // - VSync（Present sync interval）与缓冲数解耦，避免“切 VSync 导致帧队列深度变化”的隐式副作用。
    // - 历史上三缓冲也用于降低 D3D12 帧等待/超时风险。
    scDesc.BufferCount = 3;
    // 阶段 1：引入深度缓冲（即便当下的全屏四边形不依赖深度测试，先把链路补齐）。
    scDesc.DepthBufferFormat = TEX_FORMAT_D32_FLOAT;

    if (backend == Backend::D3D12) {
        auto* factory = GetEngineFactoryD3D12();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryD3D12() 返回空。");
            return false;
        }

        EngineD3D12CreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif
        // 增加 GPU 描述符堆大小，避免动态描述符耗尽
        engineCI.GPUDescriptorHeapDynamicSize[0] = 32768; // CBV_SRV_UAV
        engineCI.GPUDescriptorHeapDynamicSize[1] = 2048;  // SAMPLER

        // 请求 Mesh Shader 特性（可选）
        // 如果硬件支持（Turing+），特性会被启用；不支持则保持禁用
        engineCI.Features.MeshShaders = DEVICE_FEATURE_STATE_OPTIONAL;

        factory->CreateDeviceAndContextsD3D12(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"D3D12 设备或上下文创建失败。");
            return false;
        }

        // 检查是否需要透明模式（Mica/Acrylic 需要 DirectComposition SwapChain）
        const bool needTransparent = (appState_ != nullptr && appState_->backdrop.useTransparent);

        if (needTransparent) {
            // 使用 DirectComposition SwapChain 实现透明窗口
            std::cout << "[DiligentBackend] Transparent mode enabled, using DirectComposition SwapChain" << std::endl;

            // 获取 D3D12 设备和命令队列
            RefCntAutoPtr<IRenderDeviceD3D12> deviceD3D12;
            device_->QueryInterface(IID_RenderDeviceD3D12,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D12**>(&deviceD3D12)));
            if (!deviceD3D12) {
                SetLastError(L"无法获取 IRenderDeviceD3D12 接口。");
                return false;
            }

            ID3D12Device* d3d12Device = deviceD3D12->GetD3D12Device();

            // 获取命令队列（通过 LockCommandQueue + ICommandQueueD3D12 接口）
            ICommandQueue* cmdQueueBase = immediateContext_->LockCommandQueue();
            if (!cmdQueueBase) {
                SetLastError(L"无法锁定命令队列。");
                return false;
            }

            RefCntAutoPtr<ICommandQueueD3D12> cmdQueueD3D12;
            cmdQueueBase->QueryInterface(
                IID_CommandQueueD3D12, reinterpret_cast<IObject**>(static_cast<ICommandQueueD3D12**>(&cmdQueueD3D12)));
            if (!cmdQueueD3D12) {
                immediateContext_->UnlockCommandQueue();
                SetLastError(L"无法获取 ICommandQueueD3D12 接口。");
                return false;
            }

            ID3D12CommandQueue* cmdQueue = cmdQueueD3D12->GetD3D12CommandQueue();

            // 初始化 DirectComposition SwapChain（D3D12 版本）
            if (!dcompSwapChain_.InitD3D12(hwnd, d3d12Device, cmdQueue, initialSize.Width, initialSize.Height, 3)) {
                immediateContext_->UnlockCommandQueue();
                SetLastError(L"DirectComposition SwapChain 初始化失败。");
                return false;
            }

            immediateContext_->UnlockCommandQueue();

            // 创建 Diligent 后缓冲 RTV
            if (!CreateDCompBackBufferRTVs()) {
                SetLastError(L"创建 DirectComposition 后缓冲 RTV 失败。");
                return false;
            }

            useDCompSwapChain_ = true;
            surfaceSize_       = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
        } else {
            // 非透明模式：使用 Diligent 标准 SwapChain
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D12(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            // 诊断：打印实际使用的颜色格式（5=SRGB, 4=UNORM）
            std::cout << "[DiligentBackend] D3D12 SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    } else if (backend == Backend::Vulkan) {
        auto* factory = GetEngineFactoryVk();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryVk() 返回空。");
            return false;
        }

        EngineVkCreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif

        // 如果需要透明模式，添加外部内存扩展以支持 D3D12-Vulkan 互操作
        // 回退：禁用 Vulkan 透明模式，强制使用标准 SwapChain
        const bool needTransparent = false; // (appState_ != nullptr && appState_->backdrop.useTransparent);
        OutputDebugStringA(needTransparent ? "[DiligentBackend] needTransparent = TRUE\n"
                                           : "[DiligentBackend] needTransparent = FALSE (Vulkan interop disabled)\n");
        std::vector<const char*> deviceExtensions;
        if (needTransparent) {
            deviceExtensions.push_back("VK_KHR_external_memory_win32");
            deviceExtensions.push_back("VK_KHR_external_semaphore_win32");
            engineCI.DeviceExtensionCount   = static_cast<Uint32>(deviceExtensions.size());
            engineCI.ppDeviceExtensionNames = deviceExtensions.data();
            OutputDebugStringA("[DiligentBackend] Vulkan transparent mode: enabling external memory extensions\n");
        }

        factory->CreateDeviceAndContextsVk(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"Vulkan 设备或上下文创建失败。");
            return false;
        }

        if (needTransparent) {
            vkD3D12Interop_ = std::make_unique<VulkanD3D12Interop>();
            if (!vkD3D12Interop_->Init(hwnd, device_, immediateContext_, scDesc.Width, scDesc.Height)) {
                std::cerr << "[DiligentBackend] Failed to init VulkanD3D12Interop" << std::endl;
                SetLastError(L"Vulkan Interop 初始化失败。");
                vkD3D12Interop_.reset();
                return false;
            }
            useVkD3D12Interop_ = true;
            surfaceSize_       = {vkD3D12Interop_->GetWidth(), vkD3D12Interop_->GetHeight()};
        } else {
            factory->CreateSwapChainVk(device_, immediateContext_, scDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            useVkD3D12Interop_      = false;
            // 诊断：打印实际使用的颜色格式（5=SRGB, 4=UNORM）
            std::cout << "[DiligentBackend] Vulkan SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    } else if (backend == Backend::D3D11) {
        // D3D11 初始化
        auto* factory = GetEngineFactoryD3D11();
        if (factory == nullptr) {
            SetLastError(L"GetEngineFactoryD3D11() 返回空。");
            return false;
        }

        EngineD3D11CreateInfo engineCI{};
#ifdef _DEBUG
        engineCI.EnableValidation = true;
#endif

        factory->CreateDeviceAndContextsD3D11(engineCI, &device_, &immediateContext_);

        if (device_ == nullptr || immediateContext_ == nullptr) {
            SetLastError(L"D3D11 设备或上下文创建失败。");
            return false;
        }

        // 检查是否需要透明模式（Mica/Acrylic 需要 DirectComposition SwapChain）
        const bool needTransparent = (appState_ != nullptr && appState_->backdrop.useTransparent);

        if (needTransparent) {
            // 使用 DirectComposition SwapChain 实现透明窗口
            std::cout << "[DiligentBackend] D3D11 Transparent mode enabled, using DirectComposition SwapChain"
                      << std::endl;

            // 获取 D3D11 设备
            RefCntAutoPtr<IRenderDeviceD3D11> deviceD3D11;
            device_->QueryInterface(IID_RenderDeviceD3D11,
                                    reinterpret_cast<IObject**>(static_cast<IRenderDeviceD3D11**>(&deviceD3D11)));
            if (!deviceD3D11) {
                SetLastError(L"无法获取 IRenderDeviceD3D11 接口。");
                return false;
            }

            ID3D11Device* d3d11Device = deviceD3D11->GetD3D11Device();

            // 初始化 DirectComposition SwapChain（D3D11 版本）
            if (!dcompSwapChain_.InitD3D11(hwnd, d3d11Device, initialSize.Width, initialSize.Height, 3)) {
                SetLastError(L"D3D11 DirectComposition SwapChain 初始化失败。");
                return false;
            }

            // 创建 Diligent 后缓冲 RTV
            if (!CreateDCompBackBufferRTVs()) {
                SetLastError(L"创建 D3D11 DirectComposition 后缓冲 RTV 失败。");
                return false;
            }

            useDCompSwapChain_ = true;
            surfaceSize_       = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
        } else {
            // 非透明模式：使用 Diligent 标准 SwapChain
            FullScreenModeDesc fsDesc{};
            factory->CreateSwapChainD3D11(device_, immediateContext_, scDesc, fsDesc, window, &swapChain_);

            if (swapChain_ == nullptr) {
                SetLastError(L"D3D11 SwapChain 创建失败。");
                return false;
            }

            const auto& scFinalDesc = swapChain_->GetDesc();
            surfaceSize_            = {scFinalDesc.Width, scFinalDesc.Height};
            // 诊断：打印实际使用的颜色格式
            std::cout << "[DiligentBackend] D3D11 SwapChain ColorBufferFormat: "
                      << static_cast<int>(scFinalDesc.ColorBufferFormat)
                      << (scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM_SRGB   ? " (RGBA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_RGBA8_UNORM      ? " (RGBA8_UNORM)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM_SRGB ? " (BGRA8_UNORM_SRGB)"
                          : scFinalDesc.ColorBufferFormat == TEX_FORMAT_BGRA8_UNORM      ? " (BGRA8_UNORM)"
                                                                                         : " (Other)")
                      << std::endl;
        }
    }

    // 注意：Win32 的 WM_SIZE/ClientRect 尺寸在某些 DPI/缩放配置下可能与 SwapChain 实际尺寸不完全一致。
    // 后续渲染/点精灵的像素尺寸换算依赖"真实 RT 尺寸"，这里 surfaceSize_ 已在上面设置。
    startTime_    = std::chrono::steady_clock::now();

    // 预编译字节码模式：着色器创建是瞬时的，不需要显示进度条
    const bool needsCompile = false;

    // 提前初始化 ImGui（用于显示编译进度条）
    hwnd_        = hwnd;
    imgui_       = std::make_unique<UI::ImGuiDiligent>();
    bool imguiOk = false;
    if (useDCompSwapChain_) {
        imguiOk = imgui_->Init(hwnd, backend, device_, TEX_FORMAT_RGBA8_UNORM, surfaceSize_.Width, surfaceSize_.Height);
    } else if (useVkD3D12Interop_ && vkD3D12Interop_) {
        imguiOk = imgui_->Init(hwnd, backend, device_, TEX_FORMAT_RGBA8_UNORM, surfaceSize_.Width, surfaceSize_.Height);
    } else {
        imguiOk = imgui_->Init(hwnd, backend, device_, swapChain_);
    }
    if (!imguiOk) {
        if (lastError_.empty()) {
            SetLastError(L"ImGui 初始化失败。");
        }
        return false;
    }

    // 进度条状态
    ShaderCompileProgress::ProgressRenderer progressRenderer;
    const int kTotalPSOSteps = 7; // FullscreenQuad, Bloom, Acrylic, Starfield, Particle, ParticleCompute, SevenSegment
    progressRenderer.SetTotal(kTotalPSOSteps);
    const bool isDarkMode    = Win32WindowManager::IsSystemDarkMode();
    auto       lastFrameTime = std::chrono::steady_clock::now();

    // 进度条渲染辅助 lambda
    auto renderProgress = [&]() {
        if (!needsCompile) {
            return; // 缓存命中时不显示进度条
        }

        auto  now     = std::chrono::steady_clock::now();
        float dt      = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;

        // 开始 ImGui 帧
        imgui_->NewFrame();
        ImGui::NewFrame();

        // 渲染进度条
        progressRenderer.Render(isDarkMode, dt);

        // 结束 ImGui 帧
        ImGui::Render();

        // 获取 RTV 并渲染
        ITextureView* pRTV = GetCurrentBackBufferRTV();
        if (pRTV) {
            imgui_->Render(immediateContext_, pRTV);
        }

        // Present
        PresentFrame(0);
    };

    // 显示初始进度
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating FullscreenQuadPSO...\n");
    if (!CreateFullscreenQuadPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateFullscreenQuadPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateFullscreenQuadPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] FullscreenQuadPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating OffscreenRenderTarget...\n");
    if (!CreateOffscreenRenderTarget(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateOffscreenRenderTarget FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateOffscreenRenderTarget() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] OffscreenRenderTarget OK\n");
    UpdateFullscreenQuadBindings();

    // Bloom / Blur 资源（用于 Bloom 合成与 UI 玻璃模糊）
    OutputDebugStringA("[DiligentBackend] Creating BloomPSO...\n");
    if (!CreateBloomPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateBloomPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateBloomPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] BloomPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();

    OutputDebugStringA("[DiligentBackend] Creating AcrylicPSO...\n");
    if (!CreateAcrylicPSO()) {
        OutputDebugStringA("[DiligentBackend] CreateAcrylicPSO FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateAcrylicPSO() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] AcrylicPSO OK\n");
    progressRenderer.IncrementCompleted();
    renderProgress();
    ;

    OutputDebugStringA("[DiligentBackend] Creating BloomTextures...\n");
    if (!CreateBloomTextures(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateBloomTextures FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateBloomTextures() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] BloomTextures OK\n");

    OutputDebugStringA("[DiligentBackend] Creating UISceneTextures...\n");
    if (!CreateUISceneTextures(surfaceSize_)) {
        OutputDebugStringA("[DiligentBackend] CreateUISceneTextures FAILED!\n");
        if (lastError_.empty()) {
            SetLastError(L"CreateUISceneTextures() 失败。");
        }
        return false;
    }
    OutputDebugStringA("[DiligentBackend] UISceneTextures OK\n");

    // 阶段 2：星空（先用 2D NDC 点列表验证 point 渲染 + 闪烁 + 混合链路）。
    // 对齐 OpenGL：基准星数固定为 5 万，LOD 仅在 Draw 时按 pixelRatio 调整绘制数量。
    if (!CreateStarfieldBuffers(kStarCountBase)) {
        if (lastError_.empty()) {
            SetLastError(L"CreateStarfieldBuffers() 失败。");
        }
        return false;
    }
    if (!CreateStarfieldPSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateStarfieldPSO() 失败。");
        }
        return false;
    }
    progressRenderer.IncrementCompleted();
    renderProgress();

    // 阶段 3（第 1 步）：粒子数据通路（优先 GPU 初始化，失败则回退到 CPU）。
    // 复刻 OpenGL 旧版默认粒子规模：120 万（视觉遮蔽/密度/"不透光感"强相关）。
    bool particleInitSuccess = false;
    if (useGPUParticleInit_) {
        particleInitSuccess = CreateParticleBuffersGPU(kParticleCountMax);
        if (!particleInitSuccess) {
            MD3::DebugLog::Instance().Add(MD3::LogLevel::Warn, "[Init] GPU particle init failed, falling back to CPU");
        }
    }
    if (!particleInitSuccess) {
        // CPU fallback
        particleInitSuccess = CreateParticleBuffers(kParticleCountMax);
    }
    if (!particleInitSuccess) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleBuffers() 失败。");
        }
        return false;
    }
    if (appState_ != nullptr) {
        if (appState_->render.particleCount == 0) {
            appState_->render.particleCount = particleCount_;
        }
        if (appState_->render.pixelRatio <= 0.0f) {
            appState_->render.pixelRatio = 1.0f;
        }
        if (!lastLodBasisValid_) {
            lastLodParticleCount_ = appState_->render.particleCount;
            lastLodPixelRatio_    = appState_->render.pixelRatio;
            lastLodBasisValid_    = true;
        }
        // 初始密度补偿：与 OpenGL 旧公式一致
        appState_->render.densityCompensation =
            ComputeDensityComp(appState_->render.particleCount, appState_->render.pixelRatio);
    }
    if (!CreateParticlePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticlePSO() 失败。");
        }
        return false;
    }
    // 尝试创建 Mesh Shader PSO（可选，失败时使用 Vertex Pulling 回退）
    CreateParticleMeshShaderPSO();
    progressRenderer.IncrementCompleted();
    renderProgress();

    if (!CreateParticleComputePSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateParticleComputePSO() 失败。");
        }
        return false;
    }
    progressRenderer.IncrementCompleted();
    renderProgress();

    // 阶段 5：七段数码管 FPS 显示
    if (!CreateSevenSegmentPSO()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateSevenSegmentPSO() 失败。");
        }
        return false;
    }
    if (!CreateSevenSegmentBuffers()) {
        if (lastError_.empty()) {
            SetLastError(L"CreateSevenSegmentBuffers() 失败。");
        }
        return false;
    }
    progressRenderer.IncrementCompleted();
    renderProgress();

    // 阶段 6：MD3 UI 系统初始化（使用 AppState 中的 DPI 缩放）
    const float dpiScale = appState_ ? appState_->window.dpiScale : 1.0f;
    MD3::Init(device_, immediateContext_, backend_, dpiScale);
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));
    MD3::ApplyImGuiStyle();

    // 从注册表加载 ImGui 布局（在第一次 NewFrame 之前）
    Settings::LoadImGuiLayout();

    // 如果启动时透明模式已开启，将辉光设为 0（保留默认值在 bloomStrengthBeforeTransp_ 中）
    // D-015 Phase B：Bloom 强度/开关迁入 state。Windows 历史上由后端成员固定默认
    // 0.5f（不随会话持久化），此处在 state 上显式建立同一默认，保持既有观感。
    if (appState_ != nullptr) {
        appState_->render.bloomEnabled     = true;
        appState_->render.bloomBlurStrength = 0.5f;
    }
    if (appState_ != nullptr && appState_->backdrop.useTransparent) {
        appState_->render.bloomBlurStrength = 0.0f;
        // bloomStrengthBeforeTransp_ 保持默认值 0.5f，用于关闭透明时恢复
    }

    // D-015 Phase B（全对称）：手部追踪（HandTracking::Controller）已上移到 Win32 外壳，
    // 由外壳在此后 Init/StartWithCameraSelector、每帧 Tick 并构建 GestureInput / 面板状态。
    return true;
}

void DiligentBackend::Shutdown() {
    // 保存会话状态到注册表（在释放资源之前）
    if (appState_ != nullptr) {
        Settings::SaveSession(*appState_, backend_);
    }

    // 先关闭 MD3（在 ImGui 之前）
    MD3::Shutdown();

    // 关闭 ImGui（在释放设备前）
    if (imgui_) {
        imgui_->Shutdown();
        imgui_.reset();
    }

    offscreenRTV_.Release();
    offscreenSRV_.Release();
    offscreenColor_.Release();
    fullscreenQuadSRB_.Release();
    fullscreenQuadPSO_.Release();

    bloomSRV_B_.Release();
    bloomRTV_B_.Release();
    bloomTexB_.Release();
    bloomSRV_A_.Release();
    bloomRTV_A_.Release();
    bloomTexA_.Release();
    bloomSRV_D_.Release();
    bloomRTV_D_.Release();
    bloomTexD_.Release();
    bloomSRV_C_.Release();
    bloomRTV_C_.Release();
    bloomTexC_.Release();
    bloomBlurConstants_.Release();
    bloomBlurSRB_.Release();
    bloomBlurPSO_.Release();
    bloomDownsampleSRB_.Release();
    bloomDownsamplePSO_.Release();
    bloomConstants_.Release();
    bloomW_  = 0;
    bloomH_  = 0;
    bloomW2_ = 0;
    bloomH2_ = 0;

    acrylicSRB_.Release();
    acrylicPSO_.Release();
    acrylicConstants_.Release();

    uiSceneSRV_.Release();
    uiSceneRTV_.Release();
    uiSceneColor_.Release();

    uiAcrylicSRV_Strong_.Release();
    uiAcrylicRTV_Strong_.Release();
    uiAcrylicStrong_.Release();
    uiAcrylicSRV_Weak_.Release();
    uiAcrylicRTV_Weak_.Release();
    uiAcrylicWeak_.Release();

    uiNoiseSRV_.Release();
    uiNoiseTex_.Release();

    logPauseIconSRV_.Release();
    logPauseIconTex_.Release();
    logResumeIconSRV_.Release();
    logResumeIconTex_.Release();

    uiBlurSRV_D_.Release();
    uiBlurRTV_D_.Release();
    uiBlurTexD_.Release();
    uiBlurSRV_C_.Release();
    uiBlurRTV_C_.Release();
    uiBlurTexC_.Release();
    uiBlurSRV_B_.Release();
    uiBlurRTV_B_.Release();
    uiBlurTexB_.Release();
    uiBlurSRV_A_.Release();
    uiBlurRTV_A_.Release();
    uiBlurTexA_.Release();
    uiBlurW_  = 0;
    uiBlurH_  = 0;
    uiBlurW2_ = 0;
    uiBlurH2_ = 0;

    starSRB_.Release();
    starVB_.Release();
    starConstants_.Release();
    starPSO_.Release();

    particleSRB_.Release();
    particleConstants_.Release();
    particlePSO_.Release();
    particleIndirectArgs_.Release();

    particleComputeSRB_.Release();
    particleComputeConstants_.Release();
    particleComputePSO_.Release();
    for (auto& v : particleUAVs_) {
        v.Release();
    }
    for (auto& v : particleSRVs_) {
        v.Release();
    }
    for (auto& b : particleBuffers_) {
        b.Release();
    }
    particleCount_ = 0;

    // 在释放 Vulkan 设备之前，先清理 VulkanD3D12Interop（它持有 native Vulkan 资源）
    if (vkD3D12Interop_) {
        vkD3D12Interop_->Shutdown();
        vkD3D12Interop_.reset();
    }
    useVkD3D12Interop_ = false;

    swapChain_.Release();
    immediateContext_.Release();
    device_.Release();
}

void DiligentBackend::Resize(SurfaceSize newSize) {
    if (!IsInitialized()) {
        return;
    }
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }

    // 检查尺寸是否变化
    if (surfaceSize_.Width == newSize.Width && surfaceSize_.Height == newSize.Height) {
        return;
    }

    if (useDCompSwapChain_ && dcompSwapChain_.IsInitialized()) {
        // DXGI ResizeBuffers 要求：必须先释放所有对旧 backbuffer 的引用，否则会返回 DXGI_ERROR_INVALID_CALL。
        // 除了 dcompSwapChain_ 内部缓存的原生 backbuffer，这里还持有 Diligent 侧包装后的纹理/RTV 引用，
        // 并且 IDeviceContext 也可能缓存"当前渲染目标"引用。
        if (immediateContext_) {
            immediateContext_->SetRenderTargets(0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE);
            immediateContext_->SetVertexBuffers(0, 0, nullptr, nullptr, RESOURCE_STATE_TRANSITION_MODE_NONE,
                                                SET_VERTEX_BUFFERS_FLAG_RESET);
            immediateContext_->SetIndexBuffer(nullptr, 0, RESOURCE_STATE_TRANSITION_MODE_NONE);
            immediateContext_->Flush();
        }

        // D3D11 native blit 使用单独的缓存 RTV，必须在 ResizeBuffers 之前释放！
        if (backend_ == Backend::D3D11) {
            d3d11CachedRTV_.Reset();
            d3d11LastBackBufferPtr_ = nullptr;
        }

        for (auto& rtv : dcompBackBufferRTVs_) {
            rtv.Release();
        }
        for (auto& buf : dcompBackBuffers_) {
            buf.Release();
        }

        // Diligent 在 D3D12 下会延迟释放底层对象（跨帧回收）；仅 Release() 指针可能不足以让 DXGI 看到引用数归零。
        // 这里强制 GPU 空闲并回收 stale resources，尽量保证 ResizeBuffers 一次成功。
        if (device_) {
            device_->IdleGPU();
            device_->ReleaseStaleResources(true);
        }

        // DirectComposition SwapChain 模式
        if (!dcompSwapChain_.Resize(newSize.Width, newSize.Height)) {
            std::cerr << "[DiligentBackend] DComp SwapChain resize failed" << std::endl;
            // ResizeBuffers 失败时必须恢复 backbuffer RTV，否则后续帧会因为 RTV 为空而“卡死”。
            if (!CreateDCompBackBufferRTVs()) {
                std::cerr << "[DiligentBackend] Failed to restore DComp back buffer RTVs after resize failure"
                          << std::endl;
            }
            return;
        }

        // 重新创建后缓冲 RTV
        if (!CreateDCompBackBufferRTVs()) {
            std::cerr << "[DiligentBackend] Failed to recreate DComp back buffer RTVs after resize" << std::endl;
            return;
        }

        surfaceSize_ = {dcompSwapChain_.GetWidth(), dcompSwapChain_.GetHeight()};
    } else if (useVkD3D12Interop_ && vkD3D12Interop_ && vkD3D12Interop_->IsInitialized()) {
        // Vulkan 互操作模式
        if (!vkD3D12Interop_->Resize(newSize.Width, newSize.Height)) {
            std::cerr << "[DiligentBackend] Vulkan Interop resize failed" << std::endl;
            return;
        }
        surfaceSize_ = {vkD3D12Interop_->GetWidth(), vkD3D12Interop_->GetHeight()};
    } else if (swapChain_) {
        // 标准 Diligent SwapChain 模式
        swapChain_->Resize(newSize.Width, newSize.Height);
        const auto& newDesc = swapChain_->GetDesc();
        surfaceSize_        = {newDesc.Width, newDesc.Height};
    }

    // SwapChain Resize 只影响后备缓冲/深度缓冲；离屏 RT 需要手动重建。
    CreateOffscreenRenderTarget(surfaceSize_);
    UpdateFullscreenQuadBindings();
    CreateBloomTextures(surfaceSize_);
    CreateUISceneTextures(surfaceSize_);

    // 更新 MD3 屏幕尺寸
    MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));

    // DWM backdrop (Mica/Acrylic) 需要在 resize 后刷新
    // 仅调用 DwmExtendFrameIntoClientArea 不够，需要重新调用 DwmSetWindowAttribute 设置 backdrop type
    if (useDCompSwapChain_ && hwnd_ != nullptr && appState_ != nullptr) {
        // 获取当前 backdrop 模式并重新应用
        if (!appState_->backdrop.availableBackdrops.empty() && appState_->backdrop.backdropIndex >= 0 &&
            appState_->backdrop.backdropIndex < static_cast<int>(appState_->backdrop.availableBackdrops.size())) {
            const int currentMode = appState_->backdrop.availableBackdrops[appState_->backdrop.backdropIndex];
            Win32WindowManager::SetBackdropMode(hwnd_, currentMode, *appState_);
        }
    }
}

void DiligentBackend::RequestResize(SurfaceSize newSize) {
    if (newSize.Width == 0 || newSize.Height == 0) {
        return;
    }
    pendingResize_    = newSize;
    hasPendingResize_ = true;
}

bool DiligentBackend::RenderFrame(const App::FrameContext& frame) {
    if (!IsInitialized()) {
        return false;
    }
    // D-002 帧高度接缝：本帧的 dt / FPS / 是否有手均由外壳交付。相机动画平滑由
    // FrameCoordinator 完成并已写入 state.scene.*，FPS 度量由外壳 FpsMeter 完成，
    // 手部追踪的 Tick 已在外壳 pollGesture() 步骤完成（HandTracking::Controller 由外壳持有）。
    frameDeltaTime_ = frame.deltaTime;
    handTracked_    = frame.handTracked;
    currentFps_     = static_cast<float>(frame.framesPerSecond);
    const float frameDt = frame.deltaTime;

    // 延迟 resize：避免在 WndProc 的 WM_SIZE 里做重资源操作导致卡顿/假死。
    // 如果 ResizeBuffers 因 DXGI_ERROR_INVALID_CALL 暂时失败，保持 pending 状态，下一帧继续尝试。
    if (hasPendingResize_) {
        const auto target = pendingResize_;
        Resize(target);
        if (surfaceSize_.Width == target.Width && surfaceSize_.Height == target.Height) {
            hasPendingResize_ = false;
        }
    }

    // 动态 LOD（对齐 OpenGL）：每 0.5s 根据平滑 FPS 自动调节粒子数 / pixelRatio，并更新密度补偿。
    if (appState_ != nullptr && frameDt > 0.0f) {
        const uint32_t prevBasisCount =
            lastLodBasisValid_ ? lastLodParticleCount_ : appState_->render.particleCount;
        const float prevBasisPR = lastLodBasisValid_ ? lastLodPixelRatio_ : appState_->render.pixelRatio;

        // 确保初始值合理（外壳未初始化 particleCount 时兜底）
        if (appState_->render.particleCount == 0) {
            appState_->render.particleCount = (particleCount_ != 0) ? particleCount_ : kParticleCountMax;
        }
        if (appState_->render.pixelRatio <= 0.0f) {
            appState_->render.pixelRatio = 1.0f;
        }

        lodUpdateTimer_ += frameDt;
        if (lodUpdateTimer_ >= 0.5f) {
            lodUpdateTimer_ = 0.0f;

            if (!appState_->lod.locked) {
                const float smoothedFps = currentFps_;

                bool particleCountChanged = false;
                bool pixelRatioChanged    = false;

                // OpenGL 版阈值与步进：
                // - 低于 38 FPS：优先降低粒子数（*0.95），降到 MIN 后再降 pixelRatio（-0.03，最低 0.7）
                // - 高于 57 FPS：优先提高 pixelRatio（+0.03，最高 1.0），再提高粒子数（*1.05，最高 MAX）
                if (smoothedFps < 38.0f) {
                    if (appState_->render.particleCount > kParticleCountMin) {
                        uint32_t newCount =
                            static_cast<uint32_t>(static_cast<float>(appState_->render.particleCount) * 0.95f);
                        newCount = std::max(newCount, kParticleCountMin);
                        if (newCount != appState_->render.particleCount) {
                            appState_->render.particleCount = newCount;
                            particleCountChanged                  = true;
                            appState_->lod.lastDecision           = 1;
                        }
                    } else if (appState_->render.pixelRatio > 0.7f) {
                        float pr = appState_->render.pixelRatio - 0.03f;
                        pr       = std::max(pr, 0.7f);
                        if (std::abs(pr - appState_->render.pixelRatio) > 1e-6f) {
                            appState_->render.pixelRatio = pr;
                            pixelRatioChanged            = true;
                            appState_->lod.lastDecision  = 2;
                        }
                    }
                } else if (smoothedFps > 57.0f) {
                    if (appState_->render.pixelRatio < 1.0f) {
                        float pr = appState_->render.pixelRatio + 0.03f;
                        pr       = std::min(pr, 1.0f);
                        if (std::abs(pr - appState_->render.pixelRatio) > 1e-6f) {
                            appState_->render.pixelRatio = pr;
                            pixelRatioChanged            = true;
                            appState_->lod.lastDecision  = 3;
                        }
                    } else if (appState_->render.particleCount < kParticleCountMax) {
                        uint32_t newCount =
                            static_cast<uint32_t>(static_cast<float>(appState_->render.particleCount) * 1.05f);
                        newCount = std::min(newCount, kParticleCountMax);
                        if (newCount != appState_->render.particleCount) {
                            appState_->render.particleCount = newCount;
                            particleCountChanged                  = true;
                            appState_->lod.lastDecision           = 4;
                        }
                    }
                } else {
                    appState_->lod.lastDecision = 0;
                }

                if (particleCountChanged || pixelRatioChanged) {
                    appState_->render.densityCompensation =
                        ComputeDensityComp(appState_->render.particleCount, appState_->render.pixelRatio);
                }
            }
        }

        // 将 UI/LOD 的 activeParticleCount 同步到后端实际渲染/Compute（particleCount_ + Indirect Args）。
        uint32_t desiredCount = appState_->render.particleCount;
        desiredCount          = std::max(desiredCount, 1u);
        desiredCount          = std::min(desiredCount, kParticleCountMax);

        if (desiredCount < kParticleCountMin) {
            desiredCount = kParticleCountMin;
        }

        if (desiredCount != appState_->render.particleCount) {
            appState_->render.particleCount = desiredCount;
        }

        if (desiredCount != particleCount_) {
            particleCount_ = desiredCount;
            if (particleIndirectArgs_ != nullptr && immediateContext_ != nullptr) {
                // args = { NumVertices(6), NumInstances(particleCount_), StartVertex(0), FirstInstance(0) }
                immediateContext_->UpdateBuffer(particleIndirectArgs_, sizeof(uint32_t), sizeof(uint32_t),
                                                &particleCount_, RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
            }
        }

        // OpenGL 版：当粒子数或 pixelRatio 发生变化时，重新推导 densityComp（保持亮度/遮蔽观感）。
        const bool basisChanged = (!lastLodBasisValid_) || desiredCount != prevBasisCount ||
                                  std::abs(appState_->render.pixelRatio - prevBasisPR) > 1e-6f;
        if (basisChanged) {
            appState_->render.densityCompensation = ComputeDensityComp(desiredCount, appState_->render.pixelRatio);
            lastLodParticleCount_         = desiredCount;
            lastLodPixelRatio_            = appState_->render.pixelRatio;
            lastLodBasisValid_            = true;
        }
    }

    // 更新崩溃诊断状态（用于 ErrorHandler 的崩溃报告/对话框）
    if (appState_ != nullptr) {
        totalFrameCount_++;
        const bool handActive = frame.handTracked; // 手部追踪已上移到外壳，本帧结果经接缝交付
        ErrorHandler::UpdateState(totalFrameCount_, appState_->render.particleCount, appState_->render.pixelRatio,
                                  handActive /*handTrackingActive*/);
    }

    // ImGui 新帧
    if (imgui_) {
        imgui_->NewFrame();

        // MD3 新帧
        MD3::BeginFrame(frameDt > 0.0f ? frameDt : (1.0f / 60.0f));
        MD3::SetDarkMode(appState_->ui.darkMode);
        MD3::SetScreenSize(static_cast<float>(surfaceSize_.Width), static_cast<float>(surfaceSize_.Height));

        // 传递 Acrylic 合成纹理给 MD3（已包含：饱和度增强 + 近似 exclusion + tint 调制）
        MD3::SetBlurTexture(appState_->ui.blurEnabled ? static_cast<void*>(uiAcrylicSRV_Strong_.RawPtr()) : nullptr,
                            appState_->ui.blurEnabled);
        // 传递次级模糊纹理（用于折叠区域 Acrylic 效果，1/12 分辨率弱模糊）
        MD3::SetBlurTexture2(appState_->ui.blurEnabled ? static_cast<void*>(uiAcrylicSRV_Weak_.RawPtr()) : nullptr);
        MD3::SetNoiseTexture(appState_->ui.blurEnabled ? static_cast<void*>(uiNoiseSRV_.RawPtr()) : nullptr);
        MD3::SetNoiseIntensity((appState_ != nullptr) ? appState_->ui.noiseIntensity : 0.01f);

        // Error dialogs（统一错误处理）
        ErrorHandler::RenderErrorDialog(frameDt);

        // 崩溃分析器窗口（使用模糊背景）
        ImTextureID crashBlurTex =
            appState_->ui.blurEnabled ? reinterpret_cast<ImTextureID>(uiAcrylicSRV_Strong_.RawPtr()) : 0;
        CrashAnalyzer::Render(appState_->ui.blurEnabled, crashBlurTex, surfaceSize_.Width, surfaceSize_.Height,
                              appState_->ui.darkMode);

        // D-015 Phase B（全对称）：MD3 面板内容在接缝之上（Win32 外壳）绘制。后端在此
        // 交付本帧 acrylic 纹理钩子，外壳据此渲染共享面板；后端不再自绘面板。
        frame.drawPanel(BuildPanelHooks());

        // MD3 帧结束
        MD3::EndFrame();
    }

    // 先清屏 SwapChain（确保深度缓冲/RT 链路始终一致），再走离屏渲染 + 拷贝。
    // 1. Clear
    RenderClear();

    // 2. Offscreen Rendering (Compute + Stars + Particles)
    RenderOffscreen();

    // 3. Bloom（bright-pass + Kawase blur）
    RenderBloom();

    // 3.5 UI Blur：先把最终显示的场景颜色解析到中间纹理，再做低分辨率模糊
    // 注意：enableBlur=false 时必须整段跳过，否则会白跑大量 blur ping-pong pass。
    const bool wantUIBlur = (appState_ != nullptr) ? appState_->ui.blurEnabled : true;
    if (wantUIBlur) {
        RenderUISceneForUI();
        RenderUIBlur();
        RenderAcrylicComposite();
    }

    // 4. Blit to Backbuffer
    // D3D11 透明模式：使用原生 D3D11 API 路径避免每帧 Diligent 纹理包装开销
    const bool useD3D11NativeBlit =
        (backend_ == Backend::D3D11 && useDCompSwapChain_ && dcompSwapChain_.IsInitialized());
    if (useD3D11NativeBlit) {
        // 首次使用时初始化原生 blit 管线
        if (!d3d11NativeBlitInitialized_) {
            InitD3D11NativeBlit();
        }
        if (d3d11NativeBlitInitialized_) {
            BlitOffscreenToBackBufferD3D11();
        } else {
            BlitOffscreenToBackBuffer();
        }
    } else {
        BlitOffscreenToBackBuffer();
    }

    // 渲染七段数码管 FPS（在 BlitOffscreenToBackBuffer 之后）
    RenderSevenSegmentFPS();

    // 渲染 ImGui（在七段数码管之后，Present 之前）
    if (imgui_) {
        ITextureView* pBackBufferRTV = GetCurrentBackBufferRTV();
        if (pBackBufferRTV != nullptr) {
            imgui_->Render(immediateContext_, pBackBufferRTV);
        }

        // 检查 ImGui 是否需要保存布局（用户移动/调整窗口后）
        ImGuiIO& io = ImGui::GetIO();
        if (io.WantSaveIniSettings) {
            Settings::SaveImGuiLayout();
            io.WantSaveIniSettings = false;
        }
    }

    // 7. Present
    // VSync：
    // - 0  : Off  -> Present(0) -> Vulkan: MAILBOX/IMMEDIATE (无帧率限制)
    // - 1  : On   -> Present(1) -> Vulkan: FIFO (严格垂直同步)
    // - -1 : Adaptive -> Present(1) -> Vulkan: FIFO_RELAXED (自适应，帧晚则撕裂)
    // 注意：Diligent Vulkan 在 SyncInterval=1 时优先选择 FIFO_RELAXED，这正是自适应 VSync 的语义。
    int presentInterval = 1;
    if (appState_ != nullptr) {
        const int mode = appState_->render.vsyncMode;
        if (mode == 0) {
            presentInterval = 0;
        } else {
            // mode == 1 (On) 或 mode == -1 (Adaptive) 都使用 SyncInterval=1
            // Vulkan 会根据硬件支持选择 FIFO_RELAXED（自适应）或 FIFO（标准）
            presentInterval = 1;
        }
    }
    PresentFrame(presentInterval);
    return true;
}

// ============================================================================
// DirectComposition SwapChain 辅助方法
// ============================================================================

// ============================================================================
// D3D11 原生 Blit 管线初始化
// 使用原生 D3D11 API 创建着色器、状态对象和采样器，避免每帧 Diligent 包装开销
// ============================================================================
// ============================================================================
// D3D11 透明模式专用的原生 Blit
// 完全使用原生 D3D11 API，避免 Diligent 纹理包装开销
// ============================================================================
} // namespace ParticleSaturn::Render
