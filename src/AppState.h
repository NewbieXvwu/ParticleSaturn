#pragma once
// AppState - 应用程序全局状态封装
//
// 【D-015 Phase C 收敛 · 2026】
// 原本 Windows（src/OpenGL + src/Diligent）持有一份独立的遗留全局状态模型，与
// macOS 重设计的 `ParticleSaturn::App::AppState`（src/app/state/AppStates.h）平行演化
// （D-005 风险）。本次重启把两模型收敛为**同一个类型**：Windows 专属状态
// （DWM 背景材质 backdrop、OpenGL 崩溃信息 gl、imgui 惰性初始化标志、按键防抖输入、
// LOD 决策码、adaptiveVSyncSupported、window.resized）已并入共享模型，此处仅保留
// `AppState` 名字作为别名，以及 Windows 目标经 GLFWwindow 用户指针存取状态的辅助函数。
// 新代码请直接使用 `ParticleSaturn::App::AppState`；`AppState` 别名仅为减少改动面而存留。

#include "app/state/AppStates.h"

// 前向声明
struct GLFWwindow;

// Windows 目标沿用的无命名空间别名（与 ParticleSaturn::App::AppState 同一类型）。
using AppState = ParticleSaturn::App::AppState;

// 从 GLFWwindow 获取 AppState 指针的辅助函数
AppState* GetAppState(GLFWwindow* window);

// 设置 AppState 到 GLFWwindow 的辅助函数
void SetAppState(GLFWwindow* window, AppState* state);
