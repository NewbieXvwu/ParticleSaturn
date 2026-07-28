#pragma once

#include <windows.h>

// 前向声明：AppState 现为 ParticleSaturn::App::AppState 的别名（D-015 Phase C 收敛）。
namespace ParticleSaturn::App { struct AppState; }
using AppState = ParticleSaturn::App::AppState;

namespace ParticleSaturn::Render {
class DiligentBackend;
}

namespace ParticleSaturn::Platform::Windows {

// ============================================================================
// Win32 输入/窗口消息映射（D-015 重启 Phase A：外壳职责从 src/Diligent/Main.cpp
// 的 WndProc 抽出）。把键盘/尺寸/DPI/系统主题消息映射到 AppState 与窗口操作，
// 行为与原 WndProc 逐分支一致。Phase C 状态收敛后再改写为面向共享 App::AppState。
//
// 返回 true 表示该消息已被处理（对应 WndProc 返回 0）；返回 false 表示未处理，
// 交回 WndProc 走默认处理。ImGui 输入转发与 WM_DESTROY 仍留在 WndProc。
// ============================================================================
bool DispatchWindowMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, AppState& state,
                           Render::DiligentBackend& backend);

} // namespace ParticleSaturn::Platform::Windows
