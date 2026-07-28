#pragma once

// D-015：本头已退化为薄垫片。面板实现提升为平台中立的共享模块
// src/ui/panel/Md3Panel.{h,cpp}（namespace ParticleSaturn::UI），macOS 与 Windows
// 外壳共用。此处把共享符号别名进 Platform::MacOS，使既有 macOS 调用点无需改限定名。
#include "ui/panel/Md3Panel.h"

namespace ParticleSaturn::Platform::MacOS {

using ParticleSaturn::UI::InstallDebugLogCapture;
using ParticleSaturn::UI::Md3PanelBackendFeatures;
using ParticleSaturn::UI::Md3PanelCallbacks;
using ParticleSaturn::UI::Md3PanelHandTrackingStatus;
using ParticleSaturn::UI::RenderMd3Panel;

} // namespace ParticleSaturn::Platform::MacOS
