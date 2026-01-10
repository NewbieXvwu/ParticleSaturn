#pragma once
// UI 管理器 - ImGui 初始化、Material You 主题、自定义控件

#include "AppState.h"
#include "Utils.h"

// UI 控件动画状态
struct UIAnimState {
    AnimFloat bgOpacity, knobPos, knobSize;
    bool      active = false;
};

extern std::unordered_map<ImGuiID, UIAnimState> g_animStates;

namespace UIManager {

// 应用 Material You 主题
void ApplyMaterialYouTheme(bool dark);

// Material Design 3 风格开关控件
bool ToggleMD3(const char* label, bool* v, float dt);

// 初始化 ImGui
void Init(GLFWwindow* window, AppState& state);

// 关闭 ImGui
void Shutdown();

} // namespace UIManager

// 主题变更回调
#ifdef _WIN32
namespace WindowManager {
void OnThemeChanged(bool isDark);
} // namespace WindowManager
#endif
