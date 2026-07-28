#pragma once

// D-015：跨平台共享的 MD3 调试/设置面板。
//
// 本模块把原 macOS 专有的 RenderMd3Panel 提升为平台中立组件（imgui + MD3 +
// AppController + DiagnosticBus，无任何 Cocoa/Win32 依赖），供 macOS 与
// Windows 外壳共用。面板只通过 AppController::Dispatch 改状态、通过回调触发平台
// 动作、按能力位/回调是否存在来显隐各控件——因此“Windows 独有”的特性（Mesh
// Shader、手部追踪 SIMD 选择、摄像头选择/序号、追踪器调试开关、错误码、星数）
// 一旦在契约里表达出来，两个平台都能呈现（谁能提供数据谁就点亮）。

#include <cstdint>
#include <functional>
#include <string>

namespace ParticleSaturn::App {
class AppController;
enum class WindowMaterial : unsigned char;
} // namespace ParticleSaturn::App

struct ImDrawList;
struct ImVec2;

namespace ParticleSaturn::UI {

struct Md3PanelCallbacks {
    std::function<void()> save;
    std::function<void()> toggleFullscreen;
    std::function<void()> showCameraSelector;
    std::function<void()> restartApplication;
    std::function<void(ParticleSaturn::App::WindowMaterial)> applyWindowMaterial;
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float rounding)> drawAcrylicBackground;
    // FPS 曲线等小区域的弱模糊背景；缺省时曲线退回纯色背景。
    std::function<void(ImDrawList*, const ImVec2&, const ImVec2&, float rounding)> drawGraphAcrylic;

    // ---- 平台可选扩展（谁能提供谁就设置；未设置的控件自动隐藏）----
    // 手部追踪 SIMD 实现选择（0=Auto,1=AVX2,2=SSE,3=Scalar）。
    std::function<void(int)> setSimdMode;
    // 追踪器摄像头调试叠加开关（区别于 AppState.ui.showCameraDebug 的场景侧开关）。
    std::function<void(bool)> setHandDebugMode;
    // D3D12 Mesh Shader 开关（仅在 features.meshShaderSupported 时点亮）。
    std::function<void(bool)> setMeshShaderEnabled;
};

struct Md3PanelHandTrackingStatus {
    enum class Tracker : std::uint8_t {
        Unavailable,
        Initializing,
        Ready,
        Failed,
    };

    Tracker tracker = Tracker::Unavailable;
    std::string errorMessage;
    std::string cameraInfo;
    bool handDetected = false;
    float rawScale = 1.0f;
    float rawRotX = 0.5f;
    float rawRotY = 0.625f;

    // ---- 平台可选扩展 ----
    int errorCode = 0;         // 追踪器失败时的错误码（<0 表示未知/未提供）。
    int selectedCamera = -1;   // 当前选中的摄像头序号（-1 表示未知）。
    // SIMD：当 simdSupported 为真时显示选择器；simdImplementation 为当前实测实现名。
    bool simdSupported = false;
    int simdMode = 0;
    std::string simdImplementation;
    // 追踪器调试叠加当前状态（仅在回调 setHandDebugMode 存在时显示开关）。
    bool debugMode = false;
};

// 面板按后端申报的能力显隐特性开关（能力单点，D-002/D-004）。
struct Md3PanelBackendFeatures {
    bool analyticParticles = false;
    bool objectShaderParticles = false;

    // ---- 平台可选扩展 ----
    bool meshShaderSupported = false;  // 硬件+后端支持 Mesh Shader（D3D12）。
    bool meshShaderEnabled = false;    // 当前是否启用（受 setMeshShaderEnabled 控制）。
    std::uint32_t starCount = 0;       // 高级区域展示的星体/几何计数（0 表示不展示）。
};

void RenderMd3Panel(ParticleSaturn::App::AppController& controller, const char* backendName, std::uint32_t fps,
                    const Md3PanelBackendFeatures& features, const Md3PanelCallbacks& callbacks,
                    const Md3PanelHandTrackingStatus& handStatus = {});

// 把 std::cout/std::cerr/std::clog 重定向进 MD3 调试日志（内容仍会写回
// 原始流）。幂等，进程内只安装一次。
void InstallDebugLogCapture();

} // namespace ParticleSaturn::UI
