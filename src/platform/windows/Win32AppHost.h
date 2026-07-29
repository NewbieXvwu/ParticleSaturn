#pragma once

#include <windows.h>

#include <string>

#include "AppState.h"
#include "RenderBackend.h"

namespace ParticleSaturn::Platform::Windows {

// ============================================================================
// Win32 应用外壳辅助（D-015 重启 Phase A：外壳职责从 src/Diligent/Main.cpp 抽出）。
// 目前收敛与窗口/DPI/后端选择相关的独立自由函数，行为与原 Main.cpp 逐字一致；
// 后续将扩展为对应 macOS `CocoaAppHost` 的 Win32 AppHost（吞窗口/全屏/DPI/backdrop）。
// ============================================================================

// 检测 Windows 版本是否支持 DirectComposition 透明窗口 + Mica。
// Win10 1809+ (Build 17763+) 起启用。
bool IsDirectCompositionSupported();

// 获取窗口的 DPI 缩放因子（相对于 96 DPI）。
float GetDpiScaleForWindow(HWND hwnd);

// 从命令行解析后端：命令行参数 > 注册表 > 默认 D3D12。
Render::Backend ParseBackendFromCmdLine(const std::wstring& cmdLine);

// 获取窗口客户区尺寸。
Render::SurfaceSize GetClientSize(HWND hwnd);

// 切换全屏/窗口化：翻转 state.window.fullscreen 并应用窗口样式/位置。
// 供 F11 快捷键与共享面板的 Fullscreen 按钮共用（D-015 Phase B）。
void ToggleFullscreen(HWND hwnd, AppState& state);

} // namespace ParticleSaturn::Platform::Windows
