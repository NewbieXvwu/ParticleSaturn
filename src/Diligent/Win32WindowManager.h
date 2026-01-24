#pragma once

#include <Windows.h>

#include "../AppState.h"

namespace ParticleSaturn::Win32WindowManager {

bool IsDwmCompositionEnabled();
bool IsSystemDarkMode();
void SetTitleBarDarkMode(HWND hwnd, bool dark);

// 0=Solid, 1=Aero, 2=Acrylic, 3=Mica（与 OpenGL 版保持一致）
const char* BackdropName(int mode);

// 探测当前系统/窗口支持的背景效果，并填充 state.backdrop.availableBackdrops（按循环顺序）
void DetectAvailableBackdrops(HWND hwnd, AppState& state);

// 应用背景效果，并同步 state.backdrop.useTransparent
void SetBackdropMode(HWND hwnd, int mode, AppState& state);

} // namespace ParticleSaturn::Win32WindowManager
