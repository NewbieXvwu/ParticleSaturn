#pragma once
// Settings - 应用程序设置管理（注册表存储）
// 统一管理所有持久化设置，包括窗口状态、UI 状态、渲染参数、ImGui 布局等

#include <cstdint>
#include <string>

#include "Diligent/RenderBackend.h"

// 前向声明
struct AppState;

namespace Settings {

// 注册表路径（复用现有路径）
constexpr wchar_t kRegKey[] = L"SOFTWARE\\ParticleSaturn";

// ============================================================================
// 基础注册表操作
// ============================================================================

// 写入 DWORD 值
void SetDWORD(const wchar_t* name, uint32_t value);

// 读取 DWORD 值（失败返回 defaultValue）
uint32_t GetDWORD(const wchar_t* name, uint32_t defaultValue);

// 写入字符串值
void SetString(const wchar_t* name, const std::string& value);

// 读取字符串值（失败返回空字符串）
std::string GetString(const wchar_t* name);

// ============================================================================
// 高级 API
// ============================================================================

// 保存完整会话状态（切换 API 或退出时调用）
void SaveSession(const AppState& state, ParticleSaturn::Render::Backend backend);

// 加载会话状态（启动时调用，命令行参数优先）
// 返回 true 表示成功加载了注册表设置
bool LoadSession(AppState& state);

// 获取保存的 Backend（返回 -1 表示未保存）
int GetSavedBackend();

// 保存 Backend
void SaveBackend(ParticleSaturn::Render::Backend backend);

// ============================================================================
// ImGui 布局
// ============================================================================

// 保存 ImGui 布局到注册表
void SaveImGuiLayout();

// 从注册表加载 ImGui 布局（在 ImGui 初始化后、第一次 NewFrame 前调用）
void LoadImGuiLayout();

// ============================================================================
// 窗口状态
// ============================================================================

struct WindowState {
    int      x          = 100;
    int      y          = 100;
    int      w          = 1280;
    int      h          = 720;
    bool     fullscreen = false;
    bool     valid      = false; // 是否成功从注册表加载
};

// 保存窗口状态
void SaveWindowState(int x, int y, int w, int h, bool fullscreen);

// 加载窗口状态
WindowState LoadWindowState();

// ============================================================================
// 进程重启（用于切换 API）
// ============================================================================

// 重启程序并切换到指定 Backend
// 返回 false 表示启动失败，调用者应继续运行
bool RestartWithBackend(ParticleSaturn::Render::Backend newBackend, const AppState& state);

} // namespace Settings
