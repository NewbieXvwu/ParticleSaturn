// MD3Theme.cpp - Material Design 3 色彩系统实现

#include <imgui.h>

#include "MD3.h"

namespace MD3 {

// 辅助函数：从十六进制创建颜色
static ImVec4 Hex(unsigned int hex, float alpha = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

MD3ColorScheme GetLightColorScheme() {
    MD3ColorScheme scheme;

    // Primary 系列 - 蓝色 #0059A6
    scheme.primary            = Hex(0x0059A6);
    scheme.onPrimary          = Hex(0xFFFFFF);
    scheme.primaryContainer   = Hex(0xD3E4FF);
    scheme.onPrimaryContainer = Hex(0x001C38);

    // Secondary 系列
    scheme.secondary            = Hex(0x535F70);
    scheme.onSecondary          = Hex(0xFFFFFF);
    scheme.secondaryContainer   = Hex(0xD7E3F8);
    scheme.onSecondaryContainer = Hex(0x101C2B);

    // Tertiary 系列
    scheme.tertiary            = Hex(0x6B5778);
    scheme.onTertiary          = Hex(0xFFFFFF);
    scheme.tertiaryContainer   = Hex(0xF3DAFF);
    scheme.onTertiaryContainer = Hex(0x251432);

    // Error 系列
    scheme.error            = Hex(0xBA1A1A);
    scheme.onError          = Hex(0xFFFFFF);
    scheme.errorContainer   = Hex(0xFFDAD6);
    scheme.onErrorContainer = Hex(0x410002);

    // Surface 系列 - 使用浅色调
    scheme.surface          = Hex(0xF9F9FC);
    scheme.surfaceDim       = Hex(0xD9D9DD);
    scheme.surfaceBright    = Hex(0xF9F9FC);
    scheme.surfaceVariant   = Hex(0xDFE2EB);
    scheme.onSurface        = Hex(0x1A1C1E);
    scheme.onSurfaceVariant = Hex(0x43474E);

    // Surface Container 层次 - 从最浅到最深
    scheme.surfaceContainerLowest  = Hex(0xFFFFFF);
    scheme.surfaceContainerLow     = Hex(0xF3F3F6);
    scheme.surfaceContainer        = Hex(0xEDEDF1);
    scheme.surfaceContainerHigh    = Hex(0xE7E8EB);
    scheme.surfaceContainerHighest = Hex(0xE2E2E5);

    // Outline 系列
    scheme.outline        = Hex(0x73777F);
    scheme.outlineVariant = Hex(0xC3C6CF);

    // 其他
    scheme.inverseSurface   = Hex(0x2F3033);
    scheme.inverseOnSurface = Hex(0xF0F0F4);
    scheme.inversePrimary   = Hex(0xA3C9FF);
    scheme.shadow           = Hex(0x000000);
    scheme.scrim            = Hex(0x000000);

    // 状态层透明度
    scheme.stateLayerHover   = 0.08f;
    scheme.stateLayerFocused = 0.12f;
    scheme.stateLayerPressed = 0.12f;
    scheme.stateLayerDragged = 0.16f;

    return scheme;
}

MD3ColorScheme GetDarkColorScheme() {
    MD3ColorScheme scheme;

    // Primary 系列 - 亮蓝色
    scheme.primary            = Hex(0xA6CFFF); // 亮蓝
    scheme.onPrimary          = Hex(0x00305B);
    scheme.primaryContainer   = Hex(0x004880);
    scheme.onPrimaryContainer = Hex(0xD3E4FF);

    // Secondary 系列
    scheme.secondary            = Hex(0xBBC7DB);
    scheme.onSecondary          = Hex(0x253140);
    scheme.secondaryContainer   = Hex(0x3C4858);
    scheme.onSecondaryContainer = Hex(0xD7E3F8);

    // Tertiary 系列
    scheme.tertiary            = Hex(0xD7BDE4);
    scheme.onTertiary          = Hex(0x3B2948);
    scheme.tertiaryContainer   = Hex(0x533F5F);
    scheme.onTertiaryContainer = Hex(0xF3DAFF);

    // Error 系列
    scheme.error            = Hex(0xFFB4AB);
    scheme.onError          = Hex(0x690005);
    scheme.errorContainer   = Hex(0x93000A);
    scheme.onErrorContainer = Hex(0xFFDAD6);

    // Surface 系列 - 使用深灰 #121214 而非纯黑
    scheme.surface          = Hex(0x121214);
    scheme.surfaceDim       = Hex(0x121214);
    scheme.surfaceBright    = Hex(0x38393C);
    scheme.surfaceVariant   = Hex(0x43474E);
    scheme.onSurface        = Hex(0xE2E2E5);
    scheme.onSurfaceVariant = Hex(0xC3C6CF);

    // Surface Container 层次 - 从最暗到最亮
    scheme.surfaceContainerLowest  = Hex(0x0D0D0F);
    scheme.surfaceContainerLow     = Hex(0x1A1C1E);
    scheme.surfaceContainer        = Hex(0x1E2022);
    scheme.surfaceContainerHigh    = Hex(0x292A2D);
    scheme.surfaceContainerHighest = Hex(0x343538);

    // Outline 系列
    scheme.outline        = Hex(0x8D9199);
    scheme.outlineVariant = Hex(0x43474E);

    // 其他
    scheme.inverseSurface   = Hex(0xE2E2E5);
    scheme.inverseOnSurface = Hex(0x2F3033);
    scheme.inversePrimary   = Hex(0x0059A6);
    scheme.shadow           = Hex(0x000000);
    scheme.scrim            = Hex(0x000000);

    // 状态层透明度
    scheme.stateLayerHover   = 0.08f;
    scheme.stateLayerFocused = 0.12f;
    scheme.stateLayerPressed = 0.12f;
    scheme.stateLayerDragged = 0.16f;

    return scheme;
}

} // namespace MD3
