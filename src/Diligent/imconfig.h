// imconfig.h for Diligent Engine version
// 与 OpenGL 版独立配置，避免体积膨胀

#pragma once

// 禁用过时函数
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

// 禁用 Demo/Debug 工具以减少体积
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS

// 启用 Material Design 3 UI 系统（如果需要）
// #define IMGUI_MD3_ENABLED
