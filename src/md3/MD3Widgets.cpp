// MD3Widgets.cpp - Material Design 3 控件实现
// Toggle, Button, Slider, Card

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include "MD3.h"

namespace MD3 {

//=============================================================================
// Toggle 开关控件
//=============================================================================

bool Toggle(const char* label, bool* v) {
    using namespace ImGui;

    auto& ctx = GetContext();
    const auto& colors = ctx.colors;
    float dpi = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto it = ctx.toggleStates.find(id);
    bool isNew = (it == ctx.toggleStates.end());
    auto& state = ctx.toggleStates[id];

    // 仅在首次创建时初始化状态
    if (isNew) {
        state.knobPosition.value = *v ? 1.0f : 0.0f;
        state.knobPosition.target = state.knobPosition.value;
        state.trackFill.value = *v ? 1.0f : 0.0f;
        state.trackFill.target = state.trackFill.value;
        state.knobScale.value = *v ? 1.0f : 0.0f;
        state.knobScale.target = state.knobScale.value;
    }

    // MD3 Toggle 尺寸
    float trackWidth = 52.0f * dpi;
    float trackHeight = 32.0f * dpi;
    float knobRadiusOff = 8.0f * dpi;   // 关闭状态旋钮半径
    float knobRadiusOn = 12.0f * dpi;   // 开启状态旋钮半径
    float trackPadding = 4.0f * dpi;

    ImVec2 pos = GetCursorScreenPos();
    ImDrawList* dl = GetWindowDrawList();

    // 创建不可见按钮用于交互
    bool pressed = InvisibleButton(label, ImVec2(trackWidth, trackHeight));
    bool hovered = IsItemHovered();

    if (pressed) {
        *v = !*v;
    }

    // 更新动画目标
    state.knobPosition.target = *v ? 1.0f : 0.0f;
    state.trackFill.target = *v ? 1.0f : 0.0f;
    state.hoverState.target = hovered ? 1.0f : 0.0f;

    // 旋钮缩放：悬停或开启时变大
    float targetKnobScale = (*v || hovered) ? 1.0f : 0.0f;
    state.knobScale.target = targetKnobScale;

    // 获取动画值（限制在 0-1 范围内，防止弹簧过冲导致颜色异常）
    float knobT = std::clamp(state.knobPosition.value, 0.0f, 1.0f);
    float fillT = std::clamp(state.trackFill.value, 0.0f, 1.0f);
    float hoverT = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float scaleT = std::clamp(state.knobScale.value, 0.0f, 1.0f);

    // 计算颜色
    ImVec4 trackColorOff = colors.surfaceContainerHighest;
    ImVec4 trackColorOn = colors.primary;
    ImVec4 trackColor = BlendColors(trackColorOff, trackColorOn, fillT);

    ImVec4 knobColorOff = colors.outline;
    ImVec4 knobColorOn = colors.onPrimary;
    ImVec4 knobColor = BlendColors(knobColorOff, knobColorOn, fillT);

    // 轨道边框颜色
    ImVec4 borderColorOff = colors.outline;
    ImVec4 borderColorOn = colors.primary;
    ImVec4 borderColor = BlendColors(borderColorOff, borderColorOn, fillT);

    // 悬停状态层 - 使用动画值 fillT 来平滑过渡颜色
    if (hoverT > 0.001f) {
        // MD3 规范：关闭状态用 onSurface，开启状态用 primary（不是 onPrimary）
        ImVec4 stateLayerOff = colors.onSurface;
        ImVec4 stateLayerOn = colors.primary;
        ImVec4 stateLayer = BlendColors(stateLayerOff, stateLayerOn, fillT);
        trackColor = ApplyStateLayer(trackColor, stateLayer, colors.stateLayerHover * hoverT);
    }

    // 绘制轨道
    float trackRadius = trackHeight * 0.5f;
    ImU32 trackCol = ColorToU32(trackColor);
    dl->AddRectFilled(pos, ImVec2(pos.x + trackWidth, pos.y + trackHeight), trackCol, trackRadius);

    // 绘制轨道边框（仅在关闭状态）
    if (fillT < 0.95f) {
        ImU32 borderCol = ColorToU32(borderColor);
        float borderAlpha = 1.0f - fillT;
        ImVec4 borderWithAlpha = borderColor;
        borderWithAlpha.w *= borderAlpha;
        dl->AddRect(pos, ImVec2(pos.x + trackWidth, pos.y + trackHeight),
                    ColorToU32(borderWithAlpha), trackRadius, 0, 2.0f * dpi);
    }

    // 计算旋钮位置和大小
    float knobRadius = knobRadiusOff + (knobRadiusOn - knobRadiusOff) * scaleT;
    float knobXStart = pos.x + trackPadding + knobRadiusOn;
    float knobXEnd = pos.x + trackWidth - trackPadding - knobRadiusOn;
    float knobX = knobXStart + (knobXEnd - knobXStart) * knobT;
    float knobY = pos.y + trackHeight * 0.5f;

    // 绘制旋钮悬停光晕 - 使用动画值 fillT 而不是布尔值 *v
    if (hoverT > 0.001f) {
        float haloRadius = knobRadius + 8.0f * dpi * hoverT;
        ImVec4 haloColorOff = colors.onSurface;
        ImVec4 haloColorOn = colors.primary;
        ImVec4 haloColor = BlendColors(haloColorOff, haloColorOn, fillT);
        haloColor.w = 0.08f * hoverT;
        dl->AddCircleFilled(ImVec2(knobX, knobY), haloRadius, ColorToU32(haloColor));
    }

    // 绘制旋钮
    ImU32 knobCol = ColorToU32(knobColor);
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, knobCol);

    // 绘制旋钮上的图标（可选：开启状态显示勾选）
    if (fillT > 0.5f) {
        float iconAlpha = (fillT - 0.5f) * 2.0f;
        ImVec4 iconColor = colors.onPrimaryContainer;
        iconColor.w = iconAlpha;
        // 更大的勾选标记
        float checkSize = knobRadius * 0.9f;
        ImVec2 checkStart(knobX - checkSize * 0.35f, knobY + checkSize * 0.05f);
        ImVec2 checkMid(knobX - checkSize * 0.05f, knobY + checkSize * 0.35f);
        ImVec2 checkEnd(knobX + checkSize * 0.4f, knobY - checkSize * 0.35f);
        dl->AddLine(checkStart, checkMid, ColorToU32(iconColor), 2.5f * dpi);
        dl->AddLine(checkMid, checkEnd, ColorToU32(iconColor), 2.5f * dpi);
    }

    // 触发 Ripple
    if (pressed) {
        TriggerRipple(id, knobX, knobY, pos.x, pos.y, trackWidth, trackHeight, trackRadius);
    }

    // 绘制标签
    SameLine();
    SetCursorPosX(GetCursorPosX() + 12.0f * dpi);
    float textHeight = GetTextLineHeight();
    float offsetY = (trackHeight - textHeight) * 0.5f;
    SetCursorPosY(GetCursorPosY() + offsetY);
    TextUnformatted(label);

    return pressed;
}

//=============================================================================
// Button 按钮控件
//=============================================================================

// 内部按钮实现
static bool ButtonInternal(const char* label, ImVec2 size, int buttonType) {
    // buttonType: 0=Filled, 1=Tonal, 2=Outlined, 3=Text
    using namespace ImGui;

    auto& ctx = GetContext();
    const auto& colors = ctx.colors;
    float dpi = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto& state = ctx.buttonStates[id];

    // 计算按钮尺寸
    ImVec2 textSize = CalcTextSize(label);
    float paddingH = 24.0f * dpi;
    float paddingV = 10.0f * dpi;
    float minHeight = 40.0f * dpi;

    if (size.x <= 0) size.x = textSize.x + paddingH * 2;
    if (size.y <= 0) size.y = std::max(textSize.y + paddingV * 2, minHeight);

    ImVec2 pos = GetCursorScreenPos();
    ImDrawList* dl = GetWindowDrawList();

    // 创建不可见按钮
    bool pressed = InvisibleButton(label, size);
    bool hovered = IsItemHovered();
    bool held = IsItemActive();

    // 更新动画目标
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    state.pressState.target = held ? 1.0f : 0.0f;

    float hoverT = state.hoverState.value;
    float pressT = state.pressState.value;

    // 根据按钮类型确定颜色
    ImVec4 bgColor, textColor, borderColor;
    float cornerRadius = 20.0f * dpi;  // MD3 全圆角

    switch (buttonType) {
        case 0: // Filled
            bgColor = colors.primary;
            textColor = colors.onPrimary;
            if (hoverT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onPrimary, colors.stateLayerHover * hoverT);
            }
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onPrimary, colors.stateLayerPressed * pressT);
            }
            break;

        case 1: // Tonal
            bgColor = colors.secondaryContainer;
            textColor = colors.onSecondaryContainer;
            if (hoverT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onSecondaryContainer, colors.stateLayerHover * hoverT);
            }
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onSecondaryContainer, colors.stateLayerPressed * pressT);
            }
            break;

        case 2: // Outlined
            bgColor = ImVec4(0, 0, 0, 0);  // 透明背景
            textColor = colors.primary;
            borderColor = colors.outline;
            if (hoverT > 0.001f) {
                bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerHover * hoverT);
            }
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerPressed * pressT);
            }
            break;

        case 3: // Text
            bgColor = ImVec4(0, 0, 0, 0);
            textColor = colors.primary;
            cornerRadius = 4.0f * dpi;  // Text 按钮圆角较小
            if (hoverT > 0.001f) {
                bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerHover * hoverT);
            }
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerPressed * pressT);
            }
            break;
    }

    // 绘制背景
    if (bgColor.w > 0.001f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          ColorToU32(bgColor), cornerRadius);
    }

    // 绘制边框（Outlined 按钮）
    if (buttonType == 2) {
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                    ColorToU32(borderColor), cornerRadius, 0, 1.0f * dpi);
    }

    // 绘制文本
    ImVec2 textPos(
        pos.x + (size.x - textSize.x) * 0.5f,
        pos.y + (size.y - textSize.y) * 0.5f
    );
    dl->AddText(textPos, ColorToU32(textColor), label);

    // 触发 Ripple
    if (pressed) {
        ImVec2 mousePos = GetIO().MousePos;
        TriggerRipple(id, mousePos.x, mousePos.y, pos.x, pos.y, size.x, size.y, cornerRadius);
    }

    return pressed;
}

bool FilledButton(const char* label, ImVec2 size) {
    return ButtonInternal(label, size, 0);
}

bool TonalButton(const char* label, ImVec2 size) {
    return ButtonInternal(label, size, 1);
}

bool OutlinedButton(const char* label, ImVec2 size) {
    return ButtonInternal(label, size, 2);
}

bool TextButton(const char* label) {
    return ButtonInternal(label, ImVec2(0, 0), 3);
}

bool Button(const char* label, ImVec2 size) {
    return FilledButton(label, size);
}

//=============================================================================
// Slider 滑块控件
//=============================================================================

bool Slider(const char* label, float* v, float min, float max, const char* format) {
    using namespace ImGui;

    auto& ctx = GetContext();
    const auto& colors = ctx.colors;
    float dpi = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto& state = ctx.sliderStates[id];

    // MD3 Slider 尺寸
    float trackHeight = 4.0f * dpi;
    float thumbRadius = 10.0f * dpi;
    float thumbRadiusHover = 14.0f * dpi;
    float sliderWidth = GetContentRegionAvail().x;
    float totalHeight = thumbRadiusHover * 2 + 8.0f * dpi;

    ImVec2 pos = GetCursorScreenPos();
    ImDrawList* dl = GetWindowDrawList();

    // 计算轨道位置
    float trackY = pos.y + totalHeight * 0.5f;
    float trackStartX = pos.x + thumbRadiusHover;
    float trackEndX = pos.x + sliderWidth - thumbRadiusHover;
    float trackLength = trackEndX - trackStartX;

    // 创建不可见按钮
    InvisibleButton(label, ImVec2(sliderWidth, totalHeight));
    bool hovered = IsItemHovered();
    bool active = IsItemActive();

    // 处理拖动
    bool changed = false;
    static bool wasDragging = false;  // 跟踪是否正在拖动

    if (active) {
        float mouseX = GetIO().MousePos.x;
        float newT = (mouseX - trackStartX) / trackLength;
        newT = std::clamp(newT, 0.0f, 1.0f);
        float newValue = min + newT * (max - min);
        if (newValue != *v) {
            *v = newValue;
            changed = true;
        }
        wasDragging = true;
    } else {
        wasDragging = false;
    }

    // 更新动画
    float t = (*v - min) / (max - min);
    state.activeTrack.target = t;
    state.hoverState.target = (hovered || active) ? 1.0f : 0.0f;
    state.thumbScale.target = active ? 1.2f : (hovered ? 1.1f : 1.0f);

    // 始终使用弹簧动画（高刚度参数确保拖动时响应迅速）
    // 限制范围防止弹簧过冲导致视觉异常
    float activeT = std::clamp(state.activeTrack.value, 0.0f, 1.0f);
    float hoverT = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float scaleT = std::clamp(state.thumbScale.value, 0.0f, 1.5f);  // scale 允许稍大

    // 绘制非活跃轨道
    ImVec4 inactiveTrackColor = colors.surfaceContainerHighest;
    dl->AddRectFilled(
        ImVec2(trackStartX, trackY - trackHeight * 0.5f),
        ImVec2(trackEndX, trackY + trackHeight * 0.5f),
        ColorToU32(inactiveTrackColor),
        trackHeight * 0.5f
    );

    // 绘制活跃轨道
    float activeEndX = trackStartX + trackLength * activeT;
    ImVec4 activeTrackColor = colors.primary;
    dl->AddRectFilled(
        ImVec2(trackStartX, trackY - trackHeight * 0.5f),
        ImVec2(activeEndX, trackY + trackHeight * 0.5f),
        ColorToU32(activeTrackColor),
        trackHeight * 0.5f
    );

    // 计算滑块位置
    float thumbX = trackStartX + trackLength * activeT;
    float currentThumbRadius = thumbRadius * scaleT;

    // 绘制滑块悬停光晕
    if (hoverT > 0.001f) {
        float haloRadius = currentThumbRadius + 10.0f * dpi * hoverT;
        ImVec4 haloColor = colors.primary;
        haloColor.w = 0.12f * hoverT;
        dl->AddCircleFilled(ImVec2(thumbX, trackY), haloRadius, ColorToU32(haloColor));
    }

    // 绘制滑块
    ImVec4 thumbColor = colors.primary;
    dl->AddCircleFilled(ImVec2(thumbX, trackY), currentThumbRadius, ColorToU32(thumbColor));

    // 绘制数值标签（悬停时显示）
    if (hoverT > 0.5f) {
        char valueText[32];
        snprintf(valueText, sizeof(valueText), format, *v);
        ImVec2 valueSize = CalcTextSize(valueText);

        float labelY = trackY - currentThumbRadius - 8.0f * dpi - valueSize.y;
        float labelX = thumbX - valueSize.x * 0.5f;

        // 标签背景
        float labelPadding = 4.0f * dpi;
        ImVec4 labelBgColor = colors.inverseSurface;
        labelBgColor.w = (hoverT - 0.5f) * 2.0f;
        dl->AddRectFilled(
            ImVec2(labelX - labelPadding, labelY - labelPadding),
            ImVec2(labelX + valueSize.x + labelPadding, labelY + valueSize.y + labelPadding),
            ColorToU32(labelBgColor),
            4.0f * dpi
        );

        // 标签文本
        ImVec4 labelTextColor = colors.inverseOnSurface;
        labelTextColor.w = (hoverT - 0.5f) * 2.0f;
        dl->AddText(ImVec2(labelX, labelY), ColorToU32(labelTextColor), valueText);
    }

    return changed;
}

//=============================================================================
// Card 卡片容器
//=============================================================================

// 卡片状态栈
static std::vector<ImVec2> s_cardPositions;
static std::vector<ImVec2> s_cardSizes;

bool BeginCard(const char* id, ImVec2 size, int elevation) {
    using namespace ImGui;

    auto& ctx = GetContext();
    const auto& colors = ctx.colors;
    float dpi = ctx.dpiScale;

    ImGuiID cardId = GetID(id);

    // 获取或创建动画状态
    auto& state = ctx.cardStates[cardId];

    // 计算尺寸
    if (size.x <= 0) size.x = GetContentRegionAvail().x;
    if (size.y <= 0) size.y = 200.0f * dpi;  // 默认高度

    ImVec2 pos = GetCursorScreenPos();
    ImDrawList* dl = GetWindowDrawList();

    // 检测悬停
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    bool hovered = IsMouseHoveringRect(bb.Min, bb.Max);

    // 更新动画
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    state.elevation.target = (float)elevation + (hovered ? 1.0f : 0.0f);

    float hoverT = state.hoverState.value;
    float elevationT = state.elevation.value;

    // MD3 圆角
    float cornerRadius = 12.0f * dpi;

    // 绘制阴影（基于 elevation）
    if (elevationT > 0.1f) {
        float shadowOffset = elevationT * 2.0f * dpi;
        float shadowBlur = elevationT * 4.0f * dpi;
        ImVec4 shadowColor = colors.shadow;
        shadowColor.w = 0.15f * (elevationT / 5.0f);

        // 简化的阴影：多层半透明矩形
        for (int i = 3; i >= 1; i--) {
            float offset = shadowOffset * (float)i / 3.0f;
            float alpha = shadowColor.w * (float)(4 - i) / 3.0f;
            ImVec4 layerColor = shadowColor;
            layerColor.w = alpha;
            dl->AddRectFilled(
                ImVec2(pos.x + offset, pos.y + offset * 1.5f),
                ImVec2(pos.x + size.x + offset, pos.y + size.y + offset * 1.5f),
                ColorToU32(layerColor),
                cornerRadius
            );
        }
    }

    // 绘制卡片背景
    ImVec4 cardBgColor;
    switch (elevation) {
        case 0: cardBgColor = colors.surfaceContainerLowest; break;
        case 1: cardBgColor = colors.surfaceContainerLow; break;
        case 2: cardBgColor = colors.surfaceContainer; break;
        case 3: cardBgColor = colors.surfaceContainerHigh; break;
        default: cardBgColor = colors.surfaceContainerHighest; break;
    }

    // 悬停状态层
    if (hoverT > 0.001f) {
        cardBgColor = ApplyStateLayer(cardBgColor, colors.onSurface, colors.stateLayerHover * hoverT);
    }

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      ColorToU32(cardBgColor), cornerRadius);

    // 保存卡片状态
    s_cardPositions.push_back(pos);
    s_cardSizes.push_back(size);

    // 开始子区域
    SetCursorScreenPos(ImVec2(pos.x + 16.0f * dpi, pos.y + 16.0f * dpi));
    BeginGroup();

    // 设置内容区域裁剪
    PushClipRect(
        ImVec2(pos.x + 8.0f * dpi, pos.y + 8.0f * dpi),
        ImVec2(pos.x + size.x - 8.0f * dpi, pos.y + size.y - 8.0f * dpi),
        true
    );

    return true;
}

void EndCard() {
    using namespace ImGui;

    PopClipRect();
    EndGroup();

    if (!s_cardPositions.empty()) {
        ImVec2 pos = s_cardPositions.back();
        ImVec2 size = s_cardSizes.back();
        s_cardPositions.pop_back();
        s_cardSizes.pop_back();

        // 移动光标到卡片底部
        SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.0f * GetContext().dpiScale));
    }
}

} // namespace MD3
