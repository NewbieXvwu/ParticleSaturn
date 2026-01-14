// MD3Widgets.cpp - Material Design 3 控件实现
// Toggle, Button, Slider, Card

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

#include "MD3.h"

namespace MD3 {

//=============================================================================
// Toggle 开关控件
//=============================================================================

bool Toggle(const char* label, bool* v) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto  it    = ctx.toggleStates.find(id);
    bool  isNew = (it == ctx.toggleStates.end());
    auto& state = ctx.toggleStates[id];

    // 仅在首次创建时初始化状态
    if (isNew) {
        state.knobPosition.value  = *v ? 1.0f : 0.0f;
        state.knobPosition.target = state.knobPosition.value;
        state.trackFill.value     = *v ? 1.0f : 0.0f;
        state.trackFill.target    = state.trackFill.value;
        state.knobScale.value     = *v ? 1.0f : 0.0f;
        state.knobScale.target    = state.knobScale.value;
    }

    // MD3 Toggle 尺寸
    float trackWidth    = 52.0f * dpi;
    float trackHeight   = 32.0f * dpi;
    float knobRadiusOff = 8.0f * dpi;  // 关闭状态旋钮半径
    float knobRadiusOn  = 12.0f * dpi; // 开启状态旋钮半径
    float trackPadding  = 4.0f * dpi;

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 创建不可见按钮用于交互
    bool pressed = InvisibleButton(label, ImVec2(trackWidth, trackHeight));
    bool hovered = IsItemHovered();

    if (pressed) {
        *v = !*v;
    }

    // 更新动画目标
    state.knobPosition.target = *v ? 1.0f : 0.0f;
    state.trackFill.target    = *v ? 1.0f : 0.0f;
    state.hoverState.target   = hovered ? 1.0f : 0.0f;

    // 旋钮缩放：悬停或开启时变大
    float targetKnobScale  = (*v || hovered) ? 1.0f : 0.0f;
    state.knobScale.target = targetKnobScale;

    // 获取动画值（限制在 0-1 范围内，防止弹簧过冲导致颜色异常）
    float knobT  = std::clamp(state.knobPosition.value, 0.0f, 1.0f);
    float fillT  = std::clamp(state.trackFill.value, 0.0f, 1.0f);
    float hoverT = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float scaleT = std::clamp(state.knobScale.value, 0.0f, 1.0f);

    // 计算颜色
    ImVec4 trackColorOff = colors.surfaceContainerHighest;
    ImVec4 trackColorOn  = colors.primary;
    ImVec4 trackColor    = BlendColors(trackColorOff, trackColorOn, fillT);

    ImVec4 knobColorOff = colors.outline;
    ImVec4 knobColorOn  = colors.onPrimary;
    ImVec4 knobColor    = BlendColors(knobColorOff, knobColorOn, fillT);

    // 轨道边框颜色
    ImVec4 borderColorOff = colors.outline;
    ImVec4 borderColorOn  = colors.primary;
    ImVec4 borderColor    = BlendColors(borderColorOff, borderColorOn, fillT);

    // 悬停状态层 - 使用动画值 fillT 来平滑过渡颜色
    if (hoverT > 0.001f) {
        // MD3 规范：关闭状态用 onSurface，开启状态用 primary（不是 onPrimary）
        ImVec4 stateLayerOff = colors.onSurface;
        ImVec4 stateLayerOn  = colors.primary;
        ImVec4 stateLayer    = BlendColors(stateLayerOff, stateLayerOn, fillT);
        trackColor           = ApplyStateLayer(trackColor, stateLayer, colors.stateLayerHover * hoverT);
    }

    // 绘制轨道
    float trackRadius = trackHeight * 0.5f;
    ImU32 trackCol    = ColorToU32(trackColor);
    dl->AddRectFilled(pos, ImVec2(pos.x + trackWidth, pos.y + trackHeight), trackCol, trackRadius);

    // 绘制轨道边框（仅在关闭状态）
    if (fillT < 0.95f) {
        ImU32  borderCol       = ColorToU32(borderColor);
        float  borderAlpha     = 1.0f - fillT;
        ImVec4 borderWithAlpha = borderColor;
        borderWithAlpha.w *= borderAlpha;
        dl->AddRect(pos, ImVec2(pos.x + trackWidth, pos.y + trackHeight), ColorToU32(borderWithAlpha), trackRadius, 0,
                    2.0f * dpi);
    }

    // 计算旋钮位置和大小
    float knobRadius = knobRadiusOff + (knobRadiusOn - knobRadiusOff) * scaleT;
    float knobXStart = pos.x + trackPadding + knobRadiusOn;
    float knobXEnd   = pos.x + trackWidth - trackPadding - knobRadiusOn;
    float knobX      = knobXStart + (knobXEnd - knobXStart) * knobT;
    float knobY      = pos.y + trackHeight * 0.5f;

    // 绘制旋钮悬停光晕 - 使用动画值 fillT 而不是布尔值 *v
    if (hoverT > 0.001f) {
        float  haloRadius   = knobRadius + 8.0f * dpi * hoverT;
        ImVec4 haloColorOff = colors.onSurface;
        ImVec4 haloColorOn  = colors.primary;
        ImVec4 haloColor    = BlendColors(haloColorOff, haloColorOn, fillT);
        haloColor.w         = 0.08f * hoverT;
        dl->AddCircleFilled(ImVec2(knobX, knobY), haloRadius, ColorToU32(haloColor));
    }

    // 绘制旋钮
    ImU32 knobCol = ColorToU32(knobColor);
    dl->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, knobCol);

    // 绘制旋钮上的图标（可选：开启状态显示勾选）
    if (fillT > 0.5f) {
        float  iconAlpha = (fillT - 0.5f) * 2.0f;
        ImVec4 iconColor = colors.onPrimaryContainer;
        iconColor.w      = iconAlpha;
        // 更大的勾选标记
        float  checkSize = knobRadius * 0.9f;
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
    float offsetY    = (trackHeight - textHeight) * 0.5f;
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

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto& state = ctx.buttonStates[id];

    // 计算按钮尺寸
    ImVec2 textSize  = CalcTextSize(label);
    float  paddingH  = 24.0f * dpi;
    float  paddingV  = 10.0f * dpi;
    float  minHeight = 40.0f * dpi;

    if (size.x <= 0) {
        size.x = textSize.x + paddingH * 2;
    }
    if (size.y <= 0) {
        size.y = std::max(textSize.y + paddingV * 2, minHeight);
    }

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 创建不可见按钮
    bool pressed = InvisibleButton(label, size);
    bool hovered = IsItemHovered();
    bool held    = IsItemActive();

    // 更新动画目标
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    state.pressState.target = held ? 1.0f : 0.0f;

    float hoverT = state.hoverState.value;
    float pressT = state.pressState.value;

    // 根据按钮类型确定颜色
    ImVec4 bgColor, textColor, borderColor;
    float  cornerRadius = 20.0f * dpi; // MD3 全圆角

    switch (buttonType) {
    case 0: // Filled
        bgColor   = colors.primary;
        textColor = colors.onPrimary;
        if (hoverT > 0.001f) {
            bgColor = ApplyStateLayer(bgColor, colors.onPrimary, colors.stateLayerHover * hoverT);
        }
        if (pressT > 0.001f) {
            bgColor = ApplyStateLayer(bgColor, colors.onPrimary, colors.stateLayerPressed * pressT);
        }
        break;

    case 1: // Tonal
        bgColor   = colors.secondaryContainer;
        textColor = colors.onSecondaryContainer;
        if (hoverT > 0.001f) {
            bgColor = ApplyStateLayer(bgColor, colors.onSecondaryContainer, colors.stateLayerHover * hoverT);
        }
        if (pressT > 0.001f) {
            bgColor = ApplyStateLayer(bgColor, colors.onSecondaryContainer, colors.stateLayerPressed * pressT);
        }
        break;

    case 2:                               // Outlined
        bgColor     = ImVec4(0, 0, 0, 0); // 透明背景
        textColor   = colors.primary;
        borderColor = colors.outline;
        if (hoverT > 0.001f) {
            bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerHover * hoverT);
        }
        if (pressT > 0.001f) {
            bgColor = ApplyStateLayer(colors.surface, colors.primary, colors.stateLayerPressed * pressT);
        }
        break;

    case 3: // Text
        bgColor      = ImVec4(0, 0, 0, 0);
        textColor    = colors.primary;
        cornerRadius = 4.0f * dpi; // Text 按钮圆角较小
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
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ColorToU32(bgColor), cornerRadius);
    }

    // 绘制边框（Outlined 按钮）
    if (buttonType == 2) {
        dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), ColorToU32(borderColor), cornerRadius, 0, 1.0f * dpi);
    }

    // 绘制文本
    ImVec2 textPos(pos.x + (size.x - textSize.x) * 0.5f, pos.y + (size.y - textSize.y) * 0.5f);
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

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto  it    = ctx.sliderStates.find(id);
    bool  isNew = (it == ctx.sliderStates.end());
    auto& state = ctx.sliderStates[id];

    // 首次创建时，直接跳转到当前值（不播放动画）
    if (isNew) {
        float t                    = (*v - min) / (max - min);
        state.activeTrack.value    = t;
        state.activeTrack.target   = t;
        state.activeTrack.velocity = 0.0f;
    }

    // MD3 Slider 尺寸
    float trackHeight      = 4.0f * dpi;
    float thumbRadius      = 10.0f * dpi;
    float thumbRadiusHover = 14.0f * dpi;
    float sliderWidth      = GetContentRegionAvail().x;
    float totalHeight      = thumbRadiusHover * 2 + 8.0f * dpi;

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 计算轨道位置
    float trackY      = pos.y + totalHeight * 0.5f;
    float trackStartX = pos.x + thumbRadiusHover;
    float trackEndX   = pos.x + sliderWidth - thumbRadiusHover;
    float trackLength = trackEndX - trackStartX;

    // 创建不可见按钮
    InvisibleButton(label, ImVec2(sliderWidth, totalHeight));
    bool hovered = IsItemHovered();
    bool active  = IsItemActive();

    // 处理拖动
    bool        changed     = false;
    static bool wasDragging = false; // 跟踪是否正在拖动

    if (active) {
        float mouseX   = GetIO().MousePos.x;
        float newT     = (mouseX - trackStartX) / trackLength;
        newT           = std::clamp(newT, 0.0f, 1.0f);
        float newValue = min + newT * (max - min);
        if (newValue != *v) {
            *v      = newValue;
            changed = true;
        }
        wasDragging = true;
    } else {
        wasDragging = false;
    }

    // 更新动画
    float t                  = (*v - min) / (max - min);
    state.activeTrack.target = t;
    state.hoverState.target  = (hovered || active) ? 1.0f : 0.0f;
    state.thumbScale.target  = active ? 1.2f : (hovered ? 1.1f : 1.0f);

    // 始终使用弹簧动画（高刚度参数确保拖动时响应迅速）
    // 限制范围防止弹簧过冲导致视觉异常
    float activeT = std::clamp(state.activeTrack.value, 0.0f, 1.0f);
    float hoverT  = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float scaleT  = std::clamp(state.thumbScale.value, 0.0f, 1.5f); // scale 允许稍大

    // 绘制非活跃轨道
    ImVec4 inactiveTrackColor = colors.surfaceContainerHighest;
    dl->AddRectFilled(ImVec2(trackStartX, trackY - trackHeight * 0.5f), ImVec2(trackEndX, trackY + trackHeight * 0.5f),
                      ColorToU32(inactiveTrackColor), trackHeight * 0.5f);

    // 绘制活跃轨道
    float  activeEndX       = trackStartX + trackLength * activeT;
    ImVec4 activeTrackColor = colors.primary;
    dl->AddRectFilled(ImVec2(trackStartX, trackY - trackHeight * 0.5f), ImVec2(activeEndX, trackY + trackHeight * 0.5f),
                      ColorToU32(activeTrackColor), trackHeight * 0.5f);

    // 计算滑块位置
    float thumbX             = trackStartX + trackLength * activeT;
    float currentThumbRadius = thumbRadius * scaleT;

    // 绘制滑块悬停光晕
    if (hoverT > 0.001f) {
        float  haloRadius = currentThumbRadius + 10.0f * dpi * hoverT;
        ImVec4 haloColor  = colors.primary;
        haloColor.w       = 0.12f * hoverT;
        dl->AddCircleFilled(ImVec2(thumbX, trackY), haloRadius, ColorToU32(haloColor));
    }

    // 绘制滑块
    ImVec4 thumbColor = colors.primary;
    dl->AddCircleFilled(ImVec2(thumbX, trackY), currentThumbRadius, ColorToU32(thumbColor));

    // 绘制数值标签（悬停时显示）
    // 使用 hovered/active 状态直接判断，避免弹簧动画过冲导致闪烁
    static std::unordered_map<ImGuiID, bool> s_labelVisible;
    bool&                                    labelVisible = s_labelVisible[id];

    // 迟滞逻辑：上升时需要 > 0.6，下降时需要 < 0.4
    if (hoverT > 0.6f) {
        labelVisible = true;
    }
    if (hoverT < 0.4f) {
        labelVisible = false;
    }

    if (labelVisible && hoverT > 0.01f) {
        char valueText[32];
        snprintf(valueText, sizeof(valueText), format, *v);
        ImVec2 valueSize = CalcTextSize(valueText);

        // 固定位置，避免动画导致的抖动
        float labelY = trackY - thumbRadius - 8.0f * dpi - valueSize.y;
        float labelX = thumbX - valueSize.x * 0.5f;

        // 使用 hoverT 控制透明度，但有最小值防止闪烁
        float alpha = std::min((hoverT - 0.4f) * 3.33f, 1.0f); // 0.4-0.7 映射到 0-1

        // 标签背景
        float  labelPadding = 4.0f * dpi;
        ImVec4 labelBgColor = colors.inverseSurface;
        labelBgColor.w      = alpha;
        dl->AddRectFilled(ImVec2(labelX - labelPadding, labelY - labelPadding),
                          ImVec2(labelX + valueSize.x + labelPadding, labelY + valueSize.y + labelPadding),
                          ColorToU32(labelBgColor), 4.0f * dpi);

        // 标签文本
        ImVec4 labelTextColor = colors.inverseOnSurface;
        labelTextColor.w      = alpha;
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

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID cardId = GetID(id);

    // 获取或创建动画状态
    auto& state = ctx.cardStates[cardId];

    // 计算尺寸
    if (size.x <= 0) {
        size.x = GetContentRegionAvail().x;
    }
    if (size.y <= 0) {
        size.y = 200.0f * dpi; // 默认高度
    }

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 检测悬停
    ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
    bool   hovered = IsMouseHoveringRect(bb.Min, bb.Max);

    // 更新动画
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    state.elevation.target  = (float)elevation + (hovered ? 1.0f : 0.0f);

    float hoverT     = state.hoverState.value;
    float elevationT = state.elevation.value;

    // MD3 圆角
    float cornerRadius = 12.0f * dpi;

    // 绘制阴影（基于 elevation）
    if (elevationT > 0.1f) {
        float  shadowOffset = elevationT * 2.0f * dpi;
        float  shadowBlur   = elevationT * 4.0f * dpi;
        ImVec4 shadowColor  = colors.shadow;
        shadowColor.w       = 0.15f * (elevationT / 5.0f);

        // 简化的阴影：多层半透明矩形
        for (int i = 3; i >= 1; i--) {
            float  offset     = shadowOffset * (float)i / 3.0f;
            float  alpha      = shadowColor.w * (float)(4 - i) / 3.0f;
            ImVec4 layerColor = shadowColor;
            layerColor.w      = alpha;
            dl->AddRectFilled(ImVec2(pos.x + offset, pos.y + offset * 1.5f),
                              ImVec2(pos.x + size.x + offset, pos.y + size.y + offset * 1.5f), ColorToU32(layerColor),
                              cornerRadius);
        }
    }

    // 绘制卡片背景
    ImVec4 cardBgColor;
    switch (elevation) {
    case 0:
        cardBgColor = colors.surfaceContainerLowest;
        break;
    case 1:
        cardBgColor = colors.surfaceContainerLow;
        break;
    case 2:
        cardBgColor = colors.surfaceContainer;
        break;
    case 3:
        cardBgColor = colors.surfaceContainerHigh;
        break;
    default:
        cardBgColor = colors.surfaceContainerHighest;
        break;
    }

    // 悬停状态层
    if (hoverT > 0.001f) {
        cardBgColor = ApplyStateLayer(cardBgColor, colors.onSurface, colors.stateLayerHover * hoverT);
    }

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ColorToU32(cardBgColor), cornerRadius);

    // 保存卡片状态
    s_cardPositions.push_back(pos);
    s_cardSizes.push_back(size);

    // 开始子区域
    SetCursorScreenPos(ImVec2(pos.x + 16.0f * dpi, pos.y + 16.0f * dpi));
    BeginGroup();

    // 设置内容区域裁剪
    PushClipRect(ImVec2(pos.x + 8.0f * dpi, pos.y + 8.0f * dpi),
                 ImVec2(pos.x + size.x - 8.0f * dpi, pos.y + size.y - 8.0f * dpi), true);

    return true;
}

void EndCard() {
    using namespace ImGui;

    PopClipRect();
    EndGroup();

    if (!s_cardPositions.empty()) {
        ImVec2 pos  = s_cardPositions.back();
        ImVec2 size = s_cardSizes.back();
        s_cardPositions.pop_back();
        s_cardSizes.pop_back();

        // 移动光标到卡片底部
        SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y + 8.0f * GetContext().dpiScale));
    }
}

//=============================================================================
// Combo 下拉框控件
//=============================================================================

// Combo 状态栈结构
struct ComboStackItem {
    ImGuiID id;
    ImVec2  position;
    ImVec2  size;
    ImVec2  contentStartPos;
    float   width;
};

static std::vector<ComboStackItem> s_comboStack;

// 绘制下拉箭头（V 形）
static void DrawDropdownArrow(ImDrawList* dl, ImVec2 center, float size, float rotation, ImU32 color) {
    // rotation: 0 = 向下 V，180 = 向上 ^
    float rad  = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(rad);
    float sinR = std::sin(rad);

    // V 形的三个点（未旋转时）
    float halfW = size * 0.5f;
    float halfH = size * 0.3f;

    ImVec2 points[3] = {
        ImVec2(-halfW, -halfH), // 左上
        ImVec2(0, halfH),       // 底部中心
        ImVec2(halfW, -halfH)   // 右上
    };

    // 旋转并平移
    for (int i = 0; i < 3; i++) {
        float x   = points[i].x * cosR - points[i].y * sinR;
        float y   = points[i].x * sinR + points[i].y * cosR;
        points[i] = ImVec2(center.x + x, center.y + y);
    }

    dl->AddPolyline(points, 3, color, 0, 2.0f);
}

// 绘制勾选标记
static void DrawCheckmark(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float  checkSize = size * 0.5f;
    ImVec2 checkStart(center.x - checkSize * 0.35f, center.y + checkSize * 0.05f);
    ImVec2 checkMid(center.x - checkSize * 0.05f, center.y + checkSize * 0.35f);
    ImVec2 checkEnd(center.x + checkSize * 0.4f, center.y - checkSize * 0.35f);
    dl->AddLine(checkStart, checkMid, color, 2.0f);
    dl->AddLine(checkMid, checkEnd, color, 2.0f);
}

bool BeginCombo(const char* label, const char* preview_value) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto& state = ctx.comboStates[id];

    // 计算尺寸 - 根据内容自适应宽度，但有最小和最大限制
    float height       = 40.0f * dpi;   // MD3 标准高度
    float cornerRadius = height * 0.5f; // 全圆角
    float arrowSize    = 12.0f * dpi;
    float padding      = 16.0f * dpi;

    // 计算基于文本的宽度
    ImVec2 textSize = CalcTextSize(preview_value);
    float  minWidth = 120.0f * dpi;
    float  maxWidth = 280.0f * dpi;
    float  width    = std::clamp(textSize.x + padding * 2 + arrowSize + 8.0f * dpi, minWidth, maxWidth);

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 创建不可见按钮
    bool clicked = InvisibleButton(label, ImVec2(width, height));
    bool hovered = IsItemHovered();

    // 获取按钮的实际屏幕位置（更准确）
    ImVec2 itemMin = GetItemRectMin();
    ImVec2 itemMax = GetItemRectMax();

    // 检查是否有 popup 打开
    char popupId[256];
    snprintf(popupId, sizeof(popupId), "##ComboPopup_%s", label);
    bool isOpen = IsPopupOpen(popupId);

    // 点击切换
    if (clicked) {
        if (isOpen) {
            CloseCurrentPopup();
        } else {
            OpenPopup(popupId);
        }
    }

    // 更新动画目标
    state.hoverState.target    = hovered ? 1.0f : 0.0f;
    state.openState.target     = isOpen ? 1.0f : 0.0f;
    state.arrowRotation.target = isOpen ? 180.0f : 0.0f;

    float hoverT   = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float openT    = std::clamp(state.openState.value, 0.0f, 1.0f);
    float arrowRot = state.arrowRotation.value;

    // 计算颜色
    ImVec4 bgColor     = colors.surfaceContainerHighest;
    ImVec4 borderColor = isOpen ? colors.primary : colors.outline;
    ImVec4 textColor   = colors.onSurface;
    ImVec4 arrowColor  = colors.onSurfaceVariant;

    // 悬停状态层
    if (hoverT > 0.001f && !isOpen) {
        bgColor = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerHover * hoverT);
    }

    // 打开时边框变粗
    float borderWidth = isOpen ? 2.0f * dpi : 1.0f * dpi;

    // 绘制背景
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ColorToU32(bgColor), cornerRadius);

    // 绘制边框
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height), ColorToU32(borderColor), cornerRadius, 0, borderWidth);

    // 绘制预览文本
    ImVec2 textPos(pos.x + padding, pos.y + (height - GetTextLineHeight()) * 0.5f);
    dl->AddText(textPos, ColorToU32(textColor), preview_value);

    // 绘制下拉箭头
    ImVec2 arrowCenter(pos.x + width - padding - arrowSize * 0.5f, pos.y + height * 0.5f);
    DrawDropdownArrow(dl, arrowCenter, arrowSize, arrowRot, ColorToU32(arrowColor));

    // 触发 Ripple - 使用 itemMin 以确保位置准确
    if (clicked) {
        ImVec2 mousePos = GetIO().MousePos;
        TriggerRipple(id, mousePos.x, mousePos.y, itemMin.x, itemMin.y, width, height, cornerRadius);
    }

    // 计算弹出位置（智能判断向上或向下）
    float maxMenuHeight = 200.0f * dpi;
    float screenBottom  = GetIO().DisplaySize.y;
    float spaceBelow    = screenBottom - itemMax.y;
    float spaceAbove    = itemMin.y;

    bool  openUpward = spaceBelow < maxMenuHeight && spaceAbove > spaceBelow;
    float popupGap   = 4.0f * dpi;

    ImVec2 popupPos;
    if (openUpward) {
        popupPos = ImVec2(itemMin.x, itemMin.y);
    } else {
        popupPos = ImVec2(itemMin.x, itemMax.y + popupGap);
    }

    // 设置 popup 样式
    float popupCornerRadius = 20.0f * dpi;
    PushStyleVar(ImGuiStyleVar_WindowRounding, popupCornerRadius);
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 8.0f * dpi));
    PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f * dpi);
    PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    PushStyleColor(ImGuiCol_PopupBg, colors.surfaceContainer);
    PushStyleColor(ImGuiCol_Border, colors.outlineVariant);

    SetNextWindowPos(popupPos, ImGuiCond_Always);
    float itemHeight = 44.0f * dpi;
    SetNextWindowSizeConstraints(ImVec2(width, itemHeight + 16.0f * dpi), ImVec2(width + 32.0f * dpi, maxMenuHeight));

    bool popupOpen = BeginPopup(popupId, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    if (popupOpen) {
        // 保存状态用于 EndCombo
        ComboStackItem stackItem;
        stackItem.id              = id;
        stackItem.position        = itemMin;
        stackItem.size            = ImVec2(width, height);
        stackItem.contentStartPos = GetCursorScreenPos();
        stackItem.width           = width;
        s_comboStack.push_back(stackItem);

        // 计算动画高度
        float openT          = std::clamp(state.openState.value, 0.0f, 1.0f);
        float animatedHeight = state.lastContentHeight * openT;

        // 如果有上次高度记录，应用 ClipRect 实现展开动画
        if (state.lastContentHeight > 0.0f) {
            ImVec2 clipMin = GetCursorScreenPos();
            ImVec2 clipMax(clipMin.x + width, clipMin.y + animatedHeight);
            PushClipRect(clipMin, clipMax, true);
        }

        // 如果是向上弹出，调整位置
        if (openUpward) {
            ImVec2 popupSize = GetWindowSize();
            SetWindowPos(ImVec2(itemMin.x, itemMin.y - popupSize.y - popupGap));
        }
    } else {
        // popup 没有打开，需要 pop styles
        PopStyleColor(2);
        PopStyleVar(4);
    }

    return popupOpen;
}

void EndCombo() {
    using namespace ImGui;

    if (s_comboStack.empty()) {
        EndPopup();
        PopStyleColor(2);
        PopStyleVar(4);
        return;
    }

    ComboStackItem stackItem = s_comboStack.back();
    s_comboStack.pop_back();

    auto& ctx   = GetContext();
    auto& state = ctx.comboStates[stackItem.id];

    // 计算内容高度
    ImVec2 contentEndPos     = GetCursorScreenPos();
    float  fullContentHeight = contentEndPos.y - stackItem.contentStartPos.y;

    // 更新记录的内容高度
    if (fullContentHeight > state.lastContentHeight) {
        state.lastContentHeight = fullContentHeight;
    }

    // 如果有 ClipRect，弹出它
    if (state.lastContentHeight > 0.0f) {
        PopClipRect();
    }

    EndPopup();
    PopStyleColor(2);
    PopStyleVar(4);
}

bool Selectable(const char* label, bool selected) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    float width        = GetContentRegionAvail().x;
    float height       = 44.0f * dpi;
    float padding      = 12.0f * dpi;
    float checkSize    = 18.0f * dpi;
    float checkSpace   = checkSize + 8.0f * dpi; // 勾选图标占用的空间
    float cornerRadius = 12.0f * dpi;            // 选项的圆角

    ImVec2 pos = GetCursorScreenPos();

    // 绘制背景（在 InvisibleButton 之前，使用 GetWindowDrawList）
    ImDrawList* dl = GetWindowDrawList();

    // 创建不可见按钮
    bool clicked = InvisibleButton(label, ImVec2(width, height));
    bool hovered = IsItemHovered();

    ImGuiID id = GetItemID();

    // 获取或创建动画状态
    auto  it    = ctx.selectableStates.find(id);
    bool  isNew = (it == ctx.selectableStates.end());
    auto& state = ctx.selectableStates[id];

    if (isNew) {
        state.hoverState.value  = hovered ? 1.0f : 0.0f;
        state.hoverState.target = state.hoverState.value;
    }

    state.lastFrameSeen     = ctx.frameIndex;
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    float hoverT            = std::clamp(state.hoverState.value, 0.0f, 1.0f);

    // 计算颜色
    ImVec4 bgColor    = ImVec4(0, 0, 0, 0);
    ImVec4 textColor  = colors.onSurface;
    ImVec4 checkColor = colors.primary;

    bool needBg = false;

    // 选中项背景（基础层）
    if (selected) {
        bgColor = ApplyStateLayer(colors.surfaceContainer, colors.primary, 0.08f);
        needBg  = true;
    }

    // 悬停状态层（淡入淡出）
    if (hoverT > 0.001f) {
        ImVec4 base = needBg ? bgColor : colors.surfaceContainer;
        bgColor     = ApplyStateLayer(base, colors.onSurface, colors.stateLayerHover * hoverT);
        needBg      = true;
    }

    // 绘制背景（带圆角）
    if (needBg) {
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ColorToU32(bgColor), cornerRadius);
    }

    // 绘制勾选标记（如果选中）
    if (selected) {
        ImVec2 checkCenter(pos.x + padding + checkSize * 0.5f, pos.y + height * 0.5f);
        DrawCheckmark(dl, checkCenter, checkSize, ColorToU32(checkColor));
    }

    // 使用 DrawList 直接绘制文本
    float  textStartX  = padding + checkSpace;
    float  textOffsetY = (height - GetTextLineHeight()) * 0.5f;
    ImVec2 textPos(pos.x + textStartX, pos.y + textOffsetY);
    dl->AddText(textPos, ColorToU32(textColor), label);

    // 设置光标位置到下一行（为下一个 Selectable 准备）
    SetCursorScreenPos(ImVec2(pos.x, pos.y + height));

    // 点击后关闭 popup
    if (clicked) {
        CloseCurrentPopup();
    }

    return clicked;
}

bool MenuItem(const char* label, bool enabled, float height_override) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    float height       = (height_override > 0.0f) ? height_override : (40.0f * dpi);
    float paddingX     = 12.0f * dpi;
    float cornerRadius = 12.0f * dpi;

    if (!enabled) {
        BeginDisabled();
    }

    ImVec2 pos   = GetCursorScreenPos();
    float  width = GetContentRegionAvail().x;
    if (width < 1.0f) {
        width = CalcTextSize(label).x + paddingX * 2.0f;
    }

    bool    clicked = InvisibleButton(label, ImVec2(width, height));
    bool    hovered = IsItemHovered();
    ImGuiID id      = GetItemID();

    // 获取或创建动画状态
    auto  it    = ctx.selectableStates.find(id);
    bool  isNew = (it == ctx.selectableStates.end());
    auto& state = ctx.selectableStates[id];

    if (isNew) {
        state.hoverState.value  = hovered ? 1.0f : 0.0f;
        state.hoverState.target = state.hoverState.value;
    }

    state.lastFrameSeen     = ctx.frameIndex;
    state.hoverState.target = hovered ? 1.0f : 0.0f;
    float hoverT            = std::clamp(state.hoverState.value, 0.0f, 1.0f);

    ImDrawList* dl = GetWindowDrawList();

    if (hoverT > 0.001f) {
        ImVec4 baseBg  = GetStyleColorVec4(ImGuiCol_PopupBg);
        ImVec4 bgColor = ApplyStateLayer(baseBg, colors.onSurface, colors.stateLayerHover * hoverT);
        dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ColorToU32(bgColor), cornerRadius);
    }

    ImVec4 textColor = colors.onSurface;
    if (!enabled) {
        textColor.w *= 0.38f;
    }

    float  textOffsetY = (height - GetTextLineHeight()) * 0.5f;
    ImVec2 textPos(pos.x + paddingX, pos.y + textOffsetY);
    dl->AddText(textPos, ColorToU32(textColor), label);

    if (!enabled) {
        EndDisabled();
        return false;
    }

    if (clicked) {
        CloseCurrentPopup();
    }

    return clicked;
}

bool Combo(const char* label, int* current_item, const char* const items[], int items_count) {
    bool        changed = false;
    const char* preview = (*current_item >= 0 && *current_item < items_count) ? items[*current_item] : "";

    if (BeginCombo(label, preview)) {
        for (int i = 0; i < items_count; i++) {
            bool selected = (*current_item == i);
            ImGui::PushID(i);
            if (Selectable(items[i], selected)) {
                *current_item = i;
                changed       = true;
            }
            ImGui::PopID();
        }
        EndCombo();
    }

    return changed;
}

//=============================================================================
// CollapsingHeader 折叠头控件
//=============================================================================

// CollapsingHeader 状态栈
struct CollapsingHeaderStackItem {
    ImGuiID id;
    ImVec2  headerPos;
    ImVec2  headerSize;
    ImVec2  contentStartPos;
    bool    isOpen;
    int     drawListChannelCount; // 保存 channel 数量
    float   contentPadding;       // 内容区域边距
};

static std::vector<CollapsingHeaderStackItem> s_collapsingHeaderStack;

// 绘制展开箭头（右侧，V 形，展开时向下）
static void DrawExpandArrow(ImDrawList* dl, ImVec2 center, float size, float rotation, ImU32 color) {
    // rotation: 0 = 向右 >，90 = 向下 v
    float rad  = rotation * 3.14159265f / 180.0f;
    float cosR = std::cos(rad);
    float sinR = std::sin(rad);

    // > 形的三个点（未旋转时，指向右）
    float halfW = size * 0.25f;
    float halfH = size * 0.4f;

    ImVec2 points[3] = {
        ImVec2(-halfW, -halfH), // 左上
        ImVec2(halfW, 0),       // 右中
        ImVec2(-halfW, halfH)   // 左下
    };

    // 旋转并平移
    for (int i = 0; i < 3; i++) {
        float x   = points[i].x * cosR - points[i].y * sinR;
        float y   = points[i].x * sinR + points[i].y * cosR;
        points[i] = ImVec2(center.x + x, center.y + y);
    }

    dl->AddPolyline(points, 3, color, 0, 2.0f);
}

bool BeginCollapsingHeader(const char* label, bool default_open) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiID id = GetID(label);

    // 获取或创建动画状态
    auto  it    = ctx.collapsingHeaderStates.find(id);
    bool  isNew = (it == ctx.collapsingHeaderStates.end());
    auto& state = ctx.collapsingHeaderStates[id];

    // 使用 ImGui 存储来持久化开关状态
    ImGuiStorage* storage = GetStateStorage();
    bool          isOpen  = storage->GetInt(id, default_open ? 1 : 0) != 0;

    // 首次创建时初始化动画
    if (isNew) {
        state.openState.value      = isOpen ? 1.0f : 0.0f;
        state.openState.target     = state.openState.value;
        state.arrowRotation.value  = isOpen ? 90.0f : 0.0f;
        state.arrowRotation.target = state.arrowRotation.value;
    }

    // 计算尺寸
    float width        = GetContentRegionAvail().x;
    float height       = 48.0f * dpi;
    float cornerRadius = 12.0f * dpi;
    float arrowSize    = 16.0f * dpi;
    float padding      = 16.0f * dpi;

    ImVec2      pos = GetCursorScreenPos();
    ImDrawList* dl  = GetWindowDrawList();

    // 使用独立的 ID 创建不可见按钮，避免与内容 ID 冲突
    PushID(label);
    bool clicked = InvisibleButton("##HeaderButton", ImVec2(width, height));
    bool hovered = IsItemHovered();
    PopID();

    // 点击切换
    if (clicked) {
        isOpen = !isOpen;
        storage->SetInt(id, isOpen ? 1 : 0);
    }

    // 更新动画目标
    state.hoverState.target    = hovered ? 1.0f : 0.0f;
    state.openState.target     = isOpen ? 1.0f : 0.0f;
    state.arrowRotation.target = isOpen ? 90.0f : 0.0f;

    float hoverT   = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float openT    = std::clamp(state.openState.value, 0.0f, 1.0f);
    float arrowRot = state.arrowRotation.value;

    // 计算颜色
    ImVec4 bgColor    = colors.surfaceContainer;
    ImVec4 textColor  = colors.onSurface;
    ImVec4 arrowColor = colors.onSurfaceVariant;

    // 悬停状态层
    if (hoverT > 0.001f) {
        bgColor = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerHover * hoverT);
    }

    // 计算头部圆角（展开时只有上圆角）
    ImDrawFlags roundingFlags;
    if (openT > 0.5f) {
        roundingFlags = ImDrawFlags_RoundCornersTop;
    } else {
        roundingFlags = ImDrawFlags_RoundCornersAll;
    }

    // 绘制头部背景
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), ColorToU32(bgColor), cornerRadius, roundingFlags);

    // 绘制文本
    ImVec2 textPos(pos.x + padding, pos.y + (height - GetTextLineHeight()) * 0.5f);
    dl->AddText(textPos, ColorToU32(textColor), label);

    // 绘制箭头（右侧）
    ImVec2 arrowCenter(pos.x + width - padding - arrowSize * 0.5f, pos.y + height * 0.5f);
    DrawExpandArrow(dl, arrowCenter, arrowSize, arrowRot, ColorToU32(arrowColor));

    // 触发 Ripple
    if (clicked) {
        ImVec2 mousePos = GetIO().MousePos;
        TriggerRipple(id, mousePos.x, mousePos.y, pos.x, pos.y, width, height, cornerRadius);
    }

    // 如果展开或正在动画，开始内容区域
    if (isOpen || openT > 0.01f) {
        // 内容区域设置
        float contentPadding = 12.0f * dpi;

        // 使用 channels 来确保背景在内容之前绘制
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1); // 切换到内容 channel

        // 保存状态到栈
        CollapsingHeaderStackItem stackItem;
        stackItem.id                   = id;
        stackItem.headerPos            = pos;
        stackItem.headerSize           = ImVec2(width, height);
        stackItem.contentStartPos      = ImVec2(pos.x, pos.y + height);
        stackItem.isOpen               = isOpen;
        stackItem.drawListChannelCount = 2;
        stackItem.contentPadding       = contentPadding;
        s_collapsingHeaderStack.push_back(stackItem);

        // 计算当前动画高度
        float animatedHeight = state.lastContentHeight * openT;

        // 始终使用 ClipRect 来控制可见区域
        PushClipRect(ImVec2(pos.x, pos.y + height), ImVec2(pos.x + width, pos.y + height + animatedHeight), true);

        // 开始内容组
        BeginGroup();
        PushID(id);

        // 添加垂直间距
        Dummy(ImVec2(0, 4.0f * dpi));

        // 使用 Indent 来缩进所有内容
        Indent(contentPadding);

        // 设置内容区域的 item width
        PushItemWidth(width - contentPadding * 2);

        return true;
    }

    return false;
}

void EndCollapsingHeader() {
    using namespace ImGui;

    if (s_collapsingHeaderStack.empty()) {
        return;
    }

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    CollapsingHeaderStackItem stackItem = s_collapsingHeaderStack.back();
    s_collapsingHeaderStack.pop_back();

    auto& state = ctx.collapsingHeaderStates[stackItem.id];
    float openT = std::clamp(state.openState.value, 0.0f, 1.0f);

    // 撤销 Indent
    Unindent(stackItem.contentPadding);

    PopItemWidth();
    PopID();
    EndGroup();

    // 计算内容区域实际大小（完整高度）
    ImVec2 contentEndPos     = GetCursorScreenPos();
    float  contentPadding    = stackItem.contentPadding;
    float  fullContentHeight = contentEndPos.y - stackItem.contentStartPos.y + contentPadding;

    // 更新记录的内容高度（仅在展开时更新，用于关闭动画）
    if (stackItem.isOpen) {
        state.lastContentHeight = fullContentHeight;
    }

    // 弹出 ClipRect
    PopClipRect();

    // 计算动画高度
    float animatedHeight = state.lastContentHeight * openT;

    // 绘制内容区域边框和背景
    float width        = stackItem.headerSize.x;
    float cornerRadius = 12.0f * dpi;
    float borderWidth  = 1.0f * dpi;

    ImVec2 contentBoxMin = stackItem.contentStartPos;
    ImVec2 contentBoxMax(stackItem.headerPos.x + width, stackItem.contentStartPos.y + animatedHeight);

    ImDrawList* dl = GetWindowDrawList();

    // 切换到背景 channel 绘制背景
    if (stackItem.drawListChannelCount > 0) {
        dl->ChannelsSetCurrent(0);
    }

    // 只有动画高度大于 0 时才绘制背景
    if (animatedHeight > 1.0f) {
        // 内容区域背景
        ImVec4 contentBgColor = colors.surfaceContainerLow;
        dl->AddRectFilled(contentBoxMin, contentBoxMax, ColorToU32(contentBgColor), cornerRadius,
                          ImDrawFlags_RoundCornersBottom);

        // 内容区域边框 - 使用 AddRect 绘制完整圆角边框
        ImVec4 borderColor = colors.outlineVariant;
        borderColor.w *= 0.5f;

        // 绘制左、右、底边框（排除顶部，因为顶部与 Header 相连）
        // 左边框
        dl->AddLine(ImVec2(contentBoxMin.x, contentBoxMin.y), ImVec2(contentBoxMin.x, contentBoxMax.y - cornerRadius),
                    ColorToU32(borderColor), borderWidth);
        // 右边框
        dl->AddLine(ImVec2(contentBoxMax.x, contentBoxMin.y), ImVec2(contentBoxMax.x, contentBoxMax.y - cornerRadius),
                    ColorToU32(borderColor), borderWidth);
        // 底部边框
        dl->AddLine(ImVec2(contentBoxMin.x + cornerRadius, contentBoxMax.y),
                    ImVec2(contentBoxMax.x - cornerRadius, contentBoxMax.y), ColorToU32(borderColor), borderWidth);

        // 绘制底部圆角弧线
        ImU32  borderColorU32 = ColorToU32(borderColor);
        ImVec2 blCenter(contentBoxMin.x + cornerRadius, contentBoxMax.y - cornerRadius); // 左下圆角中心
        ImVec2 brCenter(contentBoxMax.x - cornerRadius, contentBoxMax.y - cornerRadius); // 右下圆角中心
        // 左下圆角（从 180° 到 270°，即 π 到 1.5π）
        dl->PathArcTo(blCenter, cornerRadius, IM_PI, IM_PI * 1.5f, 8);
        dl->PathStroke(borderColorU32, ImDrawFlags_None, borderWidth);
        // 右下圆角（从 270° 到 360°，即 1.5π 到 2π）
        dl->PathArcTo(brCenter, cornerRadius, IM_PI * 1.5f, IM_PI * 2.0f, 8);
        dl->PathStroke(borderColorU32, ImDrawFlags_None, borderWidth);
    }

    // 合并 channels
    if (stackItem.drawListChannelCount > 0) {
        dl->ChannelsMerge();
    }

    // 设置光标到动画高度位置（让下方元素跟随移动）
    float spacing = 8.0f * dpi;
    SetCursorScreenPos(ImVec2(stackItem.headerPos.x, stackItem.contentStartPos.y + animatedHeight + spacing));
    // 添加 Dummy 以正确扩展窗口边界（修复 ImGui SetCursorPos 警告）
    Dummy(ImVec2(0, 0));
}

//=============================================================================
// Window 窗口控件（Chrome OS 风格标题栏）
//=============================================================================

// 窗口状态栈
struct WindowStackItem {
    ImGuiID id;
    ImVec2  windowPos;
    ImVec2  windowSize;
    float   titleBarHeight;
    float   contentPadding;
};

static std::vector<WindowStackItem> s_windowStack;

// 绘制关闭按钮 X 图标
static void DrawCloseIcon(ImDrawList* dl, ImVec2 center, float size, ImU32 color) {
    float half = size * 0.5f;
    dl->AddLine(ImVec2(center.x - half, center.y - half), ImVec2(center.x + half, center.y + half), color, 1.5f);
    dl->AddLine(ImVec2(center.x + half, center.y - half), ImVec2(center.x - half, center.y + half), color, 1.5f);
}

bool BeginWindow(const char* title, bool* p_open, int flags) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    // 窗口参数
    float titleBarHeight    = 40.0f * dpi;
    float cornerRadius      = 12.0f * dpi;
    float contentPadding    = 16.0f * dpi;
    float closeButtonSize   = 24.0f * dpi;
    float closeButtonMargin = 8.0f * dpi;
    float closeIconSize     = 10.0f * dpi;

    // 使用窗口名称生成稳定的 ID（在 Begin 之前）
    ImGuiID windowId = ImHashStr(title);
    auto&   state    = ctx.windowStates[windowId];

    // 处理窗口生命周期状态
    using LifecycleState = WindowAnimState::LifecycleState;

    // 首帧初始化：设置动画初始值并开始打开动画
    if (state.firstFrame) {
        state.firstFrame     = false;
        state.lifecycleState = LifecycleState::Opening;
        // 设置初始值（关闭状态）
        state.scale.value      = 0.85f;
        state.scale.velocity   = 0.0f;
        state.offsetY.value    = 12.0f;
        state.offsetY.velocity = 0.0f;
        state.alpha.value      = 0.0f;
        state.alpha.velocity   = 0.0f;
        // 设置目标值（打开状态）
        state.scale.target   = 1.0f;
        state.offsetY.target = 0.0f;
        state.alpha.target   = 1.0f;
    }

    // 处理关闭请求
    if (state.wantClose && state.lifecycleState == LifecycleState::Open) {
        state.lifecycleState = LifecycleState::Closing;
        // 关闭动画：更高刚度，无回弹
        state.scale.stiffness   = 550.0f;
        state.scale.damping     = 38.0f;
        state.offsetY.stiffness = 550.0f;
        state.offsetY.damping   = 38.0f;
        state.alpha.stiffness   = 600.0f;
        state.alpha.damping     = 40.0f;
        // 设置关闭目标
        state.scale.target   = 0.90f;
        state.offsetY.target = 8.0f;
        state.alpha.target   = 0.0f;
    }

    // 检查动画是否完成
    float scaleSettledThreshold = 0.005f;
    float alphaSettledThreshold = 0.01f;
    bool  animationSettled      = state.scale.IsSettled(scaleSettledThreshold) &&
                            state.offsetY.IsSettled(scaleSettledThreshold) &&
                            state.alpha.IsSettled(alphaSettledThreshold);

    if (state.lifecycleState == LifecycleState::Opening && animationSettled) {
        state.lifecycleState = LifecycleState::Open;
        // 确保值精确
        state.scale.SnapToTarget();
        state.offsetY.SnapToTarget();
        state.alpha.SnapToTarget();
    } else if (state.lifecycleState == LifecycleState::Closing && animationSettled) {
        state.lifecycleState = LifecycleState::Closed;
        state.wantClose      = false;
        // 通知调用者窗口已关闭
        if (p_open) {
            *p_open = false;
        }
        // 重置状态以便下次打开
        state.firstFrame = true;
        return false;
    }

    // 获取动画值
    float scaleValue   = std::clamp(state.scale.value, 0.0f, 1.1f); // 允许轻微过冲
    float offsetYValue = state.offsetY.value;
    float alphaValue   = std::clamp(state.alpha.value, 0.0f, 1.0f);

    // 强制添加 NoTitleBar 标志
    ImGuiWindowFlags windowFlags = flags | ImGuiWindowFlags_NoTitleBar;

    // 设置窗口样式
    PushStyleVar(ImGuiStyleVar_WindowRounding, cornerRadius);
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // 应用透明度到窗口背景
    ImVec4 bgColor = colors.surfaceContainerLow;
    bgColor.w *= alphaValue;
    PushStyleColor(ImGuiCol_WindowBg, bgColor);

    // 开始 ImGui 窗口
    bool windowVisible = Begin(title, nullptr, windowFlags);

    if (!windowVisible) {
        PopStyleColor(1);
        PopStyleVar(3);
        End();
        return false;
    }

    ImVec2      windowPos  = GetWindowPos();
    ImVec2      windowSize = GetWindowSize();
    ImDrawList* dl         = GetWindowDrawList();

    // 计算缩放变换（以窗口中心为原点）
    ImVec2 windowCenter(windowPos.x + windowSize.x * 0.5f, windowPos.y + windowSize.y * 0.5f);
    float  scaledWidth  = windowSize.x * scaleValue;
    float  scaledHeight = windowSize.y * scaleValue;
    ImVec2 scaledPos(windowCenter.x - scaledWidth * 0.5f, windowCenter.y - scaledHeight * 0.5f + offsetYValue * dpi);

    // 应用缩放：调整绘制区域
    // 注意：ImGui 窗口本身不支持直接缩放，我们通过绘制层模拟
    // 对于完整的缩放效果，需要使用变换矩阵或调整所有绘制坐标

    // 绘制标题文字（居中）
    ImVec4 titleColor = colors.onSurface;
    titleColor.w *= alphaValue;
    ImVec2 titleSize = CalcTextSize(title);
    ImVec2 titlePos(windowPos.x + (windowSize.x - titleSize.x) * 0.5f,
                    windowPos.y + (titleBarHeight - titleSize.y) * 0.5f);
    dl->AddText(titlePos, ColorToU32(titleColor), title);

    // 绘制关闭按钮（如果 p_open 不为 nullptr）
    if (p_open != nullptr) {
        ImVec2 closeButtonPos(windowPos.x + windowSize.x - closeButtonMargin - closeButtonSize,
                              windowPos.y + (titleBarHeight - closeButtonSize) * 0.5f);
        ImVec2 closeButtonCenter(closeButtonPos.x + closeButtonSize * 0.5f, closeButtonPos.y + closeButtonSize * 0.5f);

        // 关闭按钮交互区域
        ImRect  closeButtonRect(closeButtonPos,
                                ImVec2(closeButtonPos.x + closeButtonSize, closeButtonPos.y + closeButtonSize));
        ImGuiID closeButtonId = GetID("##CloseButton");

        bool closeHovered = IsMouseHoveringRect(closeButtonRect.Min, closeButtonRect.Max);
        bool closePressed = closeHovered && IsMouseClicked(0);
        bool closeHeld    = closeHovered && IsMouseDown(0);

        // 更新动画目标
        state.closeButtonHover.target = closeHovered ? 1.0f : 0.0f;
        state.closeButtonPress.target = closeHeld ? 1.0f : 0.0f;

        float hoverT = std::clamp(state.closeButtonHover.value, 0.0f, 1.0f);
        float pressT = std::clamp(state.closeButtonPress.value, 0.0f, 1.0f);

        // 绘制关闭按钮背景（悬停时显示）
        if (hoverT > 0.001f) {
            ImVec4 bgColor = colors.surfaceContainerHighest;
            bgColor        = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerHover * hoverT);
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerPressed * pressT);
            }
            dl->AddCircleFilled(closeButtonCenter, closeButtonSize * 0.5f, ColorToU32(bgColor));
        }

        // 绘制关闭图标
        ImVec4 iconColor = colors.onSurfaceVariant;
        // 悬停时图标颜色加深
        if (hoverT > 0.5f) {
            iconColor = colors.onSurface;
        }
        DrawCloseIcon(dl, closeButtonCenter, closeIconSize, ColorToU32(iconColor));

        // 处理关闭事件
        if (closePressed) {
            *p_open = false;
        }
    }

    // 创建标题栏拖拽区域（排除关闭按钮区域）
    float dragAreaWidth = windowSize.x;
    if (p_open != nullptr) {
        dragAreaWidth -= (closeButtonSize + closeButtonMargin * 2);
    }

    SetCursorScreenPos(windowPos);
    InvisibleButton("##TitleBarDrag", ImVec2(dragAreaWidth, titleBarHeight));

    // 处理窗口拖动
    if (IsItemActive() && IsMouseDragging(0)) {
        ImVec2 delta = GetIO().MouseDelta;
        SetWindowPos(ImVec2(windowPos.x + delta.x, windowPos.y + delta.y));
    }

    // 保存窗口状态到栈
    WindowStackItem stackItem;
    stackItem.id             = windowId;
    stackItem.windowPos      = windowPos;
    stackItem.windowSize     = windowSize;
    stackItem.titleBarHeight = titleBarHeight;
    stackItem.contentPadding = contentPadding;
    s_windowStack.push_back(stackItem);

    // 设置内容区域起始位置
    SetCursorScreenPos(ImVec2(windowPos.x + contentPadding, windowPos.y + titleBarHeight + 8.0f * dpi));

    // 设置内容区域宽度
    PushItemWidth(windowSize.x - contentPadding * 2);

    return true;
}

void EndWindow() {
    using namespace ImGui;

    PopItemWidth();

    if (!s_windowStack.empty()) {
        s_windowStack.pop_back();
    }

    PopStyleColor(1);
    PopStyleVar(3);

    End();
}

float WindowTitleBarSpace() {
    using namespace ImGui;

    auto& ctx            = GetContext();
    float dpi            = ctx.dpiScale;
    float titleBarHeight = 40.0f * dpi;

    // 添加一个 Dummy 来占据标题栏空间
    Dummy(ImVec2(GetWindowSize().x, titleBarHeight));

    return titleBarHeight;
}

void WindowTitleBar(const char* title, bool* p_open) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    // 标题栏参数
    float titleBarHeight    = 40.0f * dpi;
    float closeButtonSize   = 24.0f * dpi;
    float closeButtonMargin = 12.0f * dpi;
    float closeIconSize     = 10.0f * dpi;

    ImGuiWindow* window = GetCurrentWindow();
    if (!window) {
        return;
    }

    ImGuiID windowId = window->ID;
    auto&   state    = ctx.windowStates[windowId];

    ImVec2      windowPos  = GetWindowPos();
    ImVec2      windowSize = GetWindowSize();
    ImDrawList* dl         = GetWindowDrawList();

    // 绘制标题栏背景（覆盖滚动内容）
    ImVec2 titleBarMin = windowPos;
    ImVec2 titleBarMax = ImVec2(windowPos.x + windowSize.x, windowPos.y + titleBarHeight);

    // 使用与窗口背景相同的颜色，或者从窗口获取
    ImVec4 titleBgColor = colors.surfaceContainerLow;
    dl->AddRectFilled(titleBarMin, titleBarMax, ColorToU32(titleBgColor), GetStyle().WindowRounding,
                      ImDrawFlags_RoundCornersTop);

    // 绘制标题文字（居中）
    ImVec2 titleSize = CalcTextSize(title);
    ImVec2 titlePos(windowPos.x + (windowSize.x - titleSize.x) * 0.5f,
                    windowPos.y + (titleBarHeight - titleSize.y) * 0.5f);
    dl->AddText(titlePos, ColorToU32(colors.onSurface), title);

    // 绘制关闭按钮（如果 p_open 不为 nullptr）
    if (p_open != nullptr) {
        ImVec2 closeButtonPos(windowPos.x + windowSize.x - closeButtonMargin - closeButtonSize,
                              windowPos.y + (titleBarHeight - closeButtonSize) * 0.5f);
        ImVec2 closeButtonCenter(closeButtonPos.x + closeButtonSize * 0.5f, closeButtonPos.y + closeButtonSize * 0.5f);

        // 关闭按钮交互区域
        ImRect closeButtonRect(closeButtonPos,
                               ImVec2(closeButtonPos.x + closeButtonSize, closeButtonPos.y + closeButtonSize));

        // 检查鼠标是否在关闭按钮区域内
        ImVec2 mousePos = GetIO().MousePos;
        bool   closeHovered =
            closeButtonRect.Contains(mousePos) && IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        bool closePressed = closeHovered && IsMouseClicked(0);
        bool closeHeld    = closeHovered && IsMouseDown(0);

        // 更新动画目标
        state.closeButtonHover.target = closeHovered ? 1.0f : 0.0f;
        state.closeButtonPress.target = closeHeld ? 1.0f : 0.0f;

        float hoverT = std::clamp(state.closeButtonHover.value, 0.0f, 1.0f);
        float pressT = std::clamp(state.closeButtonPress.value, 0.0f, 1.0f);

        // 绘制关闭按钮背景（悬停时显示）
        if (hoverT > 0.001f) {
            ImVec4 bgColor = colors.surfaceContainerHighest;
            bgColor        = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerHover * hoverT);
            if (pressT > 0.001f) {
                bgColor = ApplyStateLayer(bgColor, colors.onSurface, colors.stateLayerPressed * pressT);
            }
            dl->AddCircleFilled(closeButtonCenter, closeButtonSize * 0.5f, ColorToU32(bgColor));
        }

        // 绘制关闭图标
        ImVec4 iconColor = colors.onSurfaceVariant;
        if (hoverT > 0.5f) {
            iconColor = colors.onSurface;
        }
        DrawCloseIcon(dl, closeButtonCenter, closeIconSize, ColorToU32(iconColor));

        // 处理关闭事件
        if (closePressed) {
            *p_open = false;
        }
    }
}

void HandleSmoothScroll(float scrollSpeed) {
    using namespace ImGui;

    if (scrollSpeed <= 0.0f) {
        return;
    }

    auto& ctx = GetContext();
    float dpi = ctx.dpiScale;

    ImGuiContext& g      = *GImGui;
    ImGuiWindow*  window = g.WheelingWindow ? g.WheelingWindow : g.HoveredWindow;
    if (!window || window->Collapsed) {
        return;
    }

    if (window->Flags & ImGuiWindowFlags_NoMouseInputs) {
        return;
    }

    float scrollMaxY = window->ScrollMax.y;
    if (scrollMaxY <= 0.0f) {
        return;
    }

    ImGuiID windowId = window->ID;
    auto&   state    = ctx.smoothScrollStates[windowId];

    if (state.lastFrameProcessed == g.FrameCount) {
        return;
    }
    state.lastFrameProcessed = g.FrameCount;

    float currentScrollY = window->Scroll.y;

    float dt = ctx.deltaTime;
    if (dt <= 0.0f) {
        dt = 1.0f / 60.0f;
    }
    dt = ImClamp(dt, 0.0f, 0.5f);

    bool  isLogChild           = (window->ChildId != 0) && (std::strstr(window->Name, "LogScroll") != nullptr);
    float effectiveScrollSpeed = scrollSpeed * (isLogChild ? 1.0f : 1.5f);

    if (!state.initialized) {
        state.initialized        = true;
        state.scrollY.value      = currentScrollY;
        state.scrollY.target     = currentScrollY;
        state.scrollY.velocity   = 0.0f;
        state.scrollY.stiffness  = isLogChild ? 2200.0f : 1900.0f;
        state.scrollY.damping    = isLogChild ? 170.0f : 150.0f;
        state.lastAppliedScrollY = currentScrollY;
    }

    // 如果某帧卡顿把弹簧积分搞成 NaN/Inf，这里必须立刻自愈：否则之后任何 wheel/target 运算都是 NaN，窗口就"死"了。
    if (!std::isfinite(currentScrollY) || !std::isfinite(state.lastAppliedScrollY) ||
        !std::isfinite(state.scrollY.value) || !std::isfinite(state.scrollY.target) ||
        !std::isfinite(state.scrollY.velocity)) {
        state.scrollY.value      = std::isfinite(currentScrollY) ? currentScrollY : 0.0f;
        state.scrollY.target     = state.scrollY.value;
        state.scrollY.velocity   = 0.0f;
        state.lastAppliedScrollY = state.scrollY.value;
    }

    const ImGuiIO& io    = g.IO;
    float          wheel = io.MouseWheel;
    if (!std::isfinite(wheel)) {
        wheel = 0.0f;
    }

    bool  hasNewRequest   = false;
    float requestedTarget = state.scrollY.target;

    // 日志窗口：持续滚轮加速（更符合“刷日志”的使用习惯）
    if (isLogChild && wheel != 0.0f && !io.KeyCtrl) {
        int   dir   = (wheel > 0.0f) ? 1 : -1;
        float now   = ctx.currentTime;
        float since = (state.lastWheelTime >= 0.0f) ? (now - state.lastWheelTime) : 1e9f;

        if (since < 0.22f && dir == state.lastWheelDir) {
            state.wheelStreak++;
        } else {
            state.wheelStreak = 1;
        }
        state.lastWheelTime = now;
        state.lastWheelDir  = dir;

        float accel = 1.0f + ImMin(1.5f, 0.15f * (float)(state.wheelStreak - 1));

        // 吃掉 ImGui 本帧的 ScrollTarget（如果有），用我们自己的加速模型覆盖它。
        if (window->ScrollTarget.y != FLT_MAX) {
            window->ScrollTarget.y = FLT_MAX;
        }

        requestedTarget = state.scrollY.target + (-wheel * effectiveScrollSpeed * accel * dpi);
        hasNewRequest   = true;
    } else if (window->ScrollTarget.y != FLT_MAX) {
        // 先吃掉 ImGui 自己产生的滚动目标（来自鼠标滚轮/SetScrollHereY/导航等）。
        requestedTarget        = window->ScrollTarget.y;
        window->ScrollTarget.y = FLT_MAX;
        hasNewRequest          = true;
    } else if (wheel != 0.0f && !io.KeyCtrl) {
        // 对于 NoScrollWithMouse 的窗口，ImGui 不会生成 ScrollTarget，我们自己算。
        requestedTarget = state.scrollY.target + (-wheel * effectiveScrollSpeed * dpi);
        hasNewRequest   = true;
    }

    if (wheel == 0.0f && std::abs(currentScrollY - state.lastAppliedScrollY) > 2.0f) {
        state.scrollY.value      = currentScrollY;
        state.scrollY.target     = currentScrollY;
        state.scrollY.velocity   = 0.0f;
        state.lastAppliedScrollY = currentScrollY;
        return;
    }

    if (hasNewRequest) {
        requestedTarget = ImClamp(requestedTarget, 0.0f, scrollMaxY);

        if (state.scrollY.IsSettled(0.01f)) {
            state.scrollY.value    = state.lastAppliedScrollY;
            state.scrollY.target   = state.lastAppliedScrollY;
            state.scrollY.velocity = 0.0f;
        }

        float diff           = requestedTarget - state.scrollY.value;
        state.scrollY.target = requestedTarget;
        state.scrollY.velocity += (diff / ImMax(dt, 1.0f / 240.0f)) * (isLogChild ? 0.05f : 0.09f);
    } else {
        state.scrollY.target = ImClamp(state.scrollY.target, 0.0f, scrollMaxY);

        if (state.scrollY.IsSettled(0.01f)) {
            state.scrollY.value      = currentScrollY;
            state.scrollY.target     = currentScrollY;
            state.scrollY.velocity   = 0.0f;
            state.lastAppliedScrollY = currentScrollY;
            return;
        }
    }

    state.scrollY.Update(dt);
    float smoothed = ImClamp(state.scrollY.value, 0.0f, scrollMaxY);
    if (!std::isfinite(smoothed)) {
        state.scrollY.value    = currentScrollY;
        state.scrollY.target   = currentScrollY;
        state.scrollY.velocity = 0.0f;
        return;
    }
    SetScrollY(window, smoothed);
    state.lastAppliedScrollY = smoothed;
}

void WindowScrollbar(float titleBarHeight) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiWindow* window = GetCurrentWindow();
    if (!window) {
        return;
    }

    // 检查是否需要滚动条
    float contentHeight = window->ContentSize.y;
    float windowHeight  = window->Size.y - titleBarHeight;

    if (contentHeight <= windowHeight) {
        return; // 不需要滚动条
    }

    ImGuiID scrollbarId = GetID("##MD3Scrollbar");
    auto&   state       = ctx.scrollbarStates[scrollbarId];

    // 滚动条参数
    float scrollbarWidthMin      = 4.0f * dpi;  // 默认宽度
    float scrollbarWidthMax      = 8.0f * dpi;  // 悬停时宽度
    float scrollbarMargin        = 4.0f * dpi;  // 距离窗口边缘
    float scrollbarPaddingTop    = 8.0f * dpi;  // 上留白
    float scrollbarPaddingBottom = 16.0f * dpi; // 下留白（避开窗口圆角）
    float minThumbHeight         = 32.0f * dpi; // 最小滑块高度

    ImVec2 windowPos  = GetWindowPos();
    ImVec2 windowSize = GetWindowSize();
    float  scrollY    = GetScrollY();
    float  scrollMaxY = GetScrollMaxY();

    // 检测滚动变化，显示滚动条
    if (std::abs(scrollY - state.lastScrollY) > 0.5f) {
        state.visibility.target = 1.0f;
        state.hideTimer         = 1.5f; // 1.5秒后开始隐藏
        state.lastScrollY       = scrollY;
    }

    // 计算滚动条区域
    float trackTop    = windowPos.y + titleBarHeight + scrollbarPaddingTop;
    float trackBottom = windowPos.y + windowSize.y - scrollbarPaddingBottom;
    float trackHeight = trackBottom - trackTop;

    // 计算滑块大小和位置
    float thumbRatio  = windowHeight / contentHeight;
    float thumbHeight = std::max(trackHeight * thumbRatio, minThumbHeight);
    float thumbTravel = trackHeight - thumbHeight;
    float scrollRatio = scrollMaxY > 0 ? scrollY / scrollMaxY : 0.0f;
    float thumbTop    = trackTop + thumbTravel * scrollRatio;

    // 计算当前滚动条宽度（基于悬停状态）
    float hoverT      = std::clamp(state.hoverState.value, 0.0f, 1.0f);
    float dragT       = std::clamp(state.dragState.value, 0.0f, 1.0f);
    float visibilityT = std::clamp(state.visibility.value, 0.0f, 1.0f);

    // 悬停或拖拽时滚动条变粗
    float currentWidth = scrollbarWidthMin + (scrollbarWidthMax - scrollbarWidthMin) * std::max(hoverT, dragT);

    // 滚动条右侧位置
    float scrollbarRight = windowPos.x + windowSize.x - scrollbarMargin;
    float scrollbarLeft  = scrollbarRight - currentWidth;

    // 滑块区域（用于交互检测，使用最大宽度以便于点击）
    ImRect thumbRect(scrollbarRight - scrollbarWidthMax - 4.0f * dpi, // 扩大点击区域
                     thumbTop, scrollbarRight + 4.0f * dpi, thumbTop + thumbHeight);

    // 轨道区域（用于点击跳转）
    ImRect trackRect(scrollbarRight - scrollbarWidthMax - 4.0f * dpi, trackTop, scrollbarRight + 4.0f * dpi,
                     trackBottom);

    // 检测交互
    ImVec2 mousePos = GetIO().MousePos;

    // 排除右下角 resize 区域（避免与 WindowResize 冲突）
    float  resizeAreaSize = 38.0f * dpi;
    ImRect resizeRect(windowPos.x + windowSize.x - resizeAreaSize, windowPos.y + windowSize.y - resizeAreaSize,
                      windowPos.x + windowSize.x, windowPos.y + windowSize.y);
    bool   inResizeArea = resizeRect.Contains(mousePos);

    bool trackHovered = trackRect.Contains(mousePos) && !inResizeArea &&
                        IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool thumbHovered = thumbRect.Contains(mousePos) && !inResizeArea &&
                        IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // 当滚动条区域悬停时，设置 HoveredId 阻止窗口 resize
    if (trackHovered || thumbHovered) {
        SetHoveredID(scrollbarId);
    }

    // 静态变量跟踪拖拽状态
    static bool    isDragging      = false;
    static float   dragStartY      = 0.0f;
    static float   dragStartScroll = 0.0f;
    static ImGuiID dragWindowId    = 0;

    // 开始拖拽
    if (thumbHovered && IsMouseClicked(0)) {
        isDragging      = true;
        dragStartY      = mousePos.y;
        dragStartScroll = scrollY;
        dragWindowId    = window->ID;
        // 设置 ActiveID 以阻止其他控件响应鼠标事件
        SetActiveID(scrollbarId, window);
    }

    // 处理拖拽
    if (isDragging && dragWindowId == window->ID) {
        if (IsMouseDown(0)) {
            float deltaY      = mousePos.y - dragStartY;
            float scrollDelta = (deltaY / thumbTravel) * scrollMaxY;
            SetScrollY(std::clamp(dragStartScroll + scrollDelta, 0.0f, scrollMaxY));

            // 保持可见
            state.visibility.target = 1.0f;
            state.hideTimer         = 1.5f;

            // 保持 ActiveID
            SetActiveID(scrollbarId, window);
        } else {
            isDragging   = false;
            dragWindowId = 0;
            // 清除 ActiveID
            if (GetActiveID() == scrollbarId) {
                ClearActiveID();
            }
        }
    }

    // 点击轨道跳转
    if (trackHovered && !thumbHovered && IsMouseClicked(0) && !isDragging) {
        float clickRatio = (mousePos.y - trackTop - thumbHeight * 0.5f) / thumbTravel;
        clickRatio       = std::clamp(clickRatio, 0.0f, 1.0f);
        SetScrollY(clickRatio * scrollMaxY);

        state.visibility.target = 1.0f;
        state.hideTimer         = 1.5f;
    }

    // 更新动画目标
    bool isActive           = isDragging && dragWindowId == window->ID;
    state.hoverState.target = (trackHovered || thumbHovered) ? 1.0f : 0.0f;
    state.dragState.target  = isActive ? 1.0f : 0.0f;

    // 悬停时保持可见
    if (trackHovered || thumbHovered || isActive) {
        state.visibility.target = 1.0f;
        state.hideTimer         = 1.5f;
    }

    // 如果完全不可见，不绘制
    if (visibilityT < 0.01f) {
        return;
    }

    // 绘制滚动条
    ImDrawList* dl = GetWindowDrawList();

    // 滑块颜色
    ImVec4 thumbColor = colors.onSurfaceVariant;
    // 基础透明度 40%，悬停时 80%
    float baseAlpha = 0.4f + 0.4f * std::max(hoverT, dragT);
    thumbColor.w    = baseAlpha * visibilityT;

    // 绘制滑块（圆角胶囊形状）
    float  thumbRadius = currentWidth * 0.5f;
    ImVec2 thumbMin(scrollbarLeft, thumbTop);
    ImVec2 thumbMax(scrollbarRight, thumbTop + thumbHeight);

    dl->AddRectFilled(thumbMin, thumbMax, ColorToU32(thumbColor), thumbRadius);
}

//=============================================================================
// Window Resize 控件（iPadOS 风格右下角圆弧）
//=============================================================================

void WindowResize(float minWidth, float minHeight) {
    using namespace ImGui;

    auto&       ctx    = GetContext();
    const auto& colors = ctx.colors;
    float       dpi    = ctx.dpiScale;

    ImGuiWindow* window = GetCurrentWindow();
    if (!window) {
        return;
    }

    ImGuiID resizeId = GetID("##MD3Resize");
    auto&   state    = ctx.resizeStates[resizeId];

    ImVec2 windowPos  = GetWindowPos();
    ImVec2 windowSize = GetWindowSize();
    ImVec2 mousePos   = GetIO().MousePos;

    // 参数
    float windowRadius = 12.0f * dpi; // 窗口圆角
    float arcInset     = -1.0f * dpi; // 圆弧距离窗口内边缘的偏移（负值=更靠近边缘）
    float arcRadius    = 17.0f * dpi; // 圆弧半径
    float arcThickness = 3.8f * dpi;  // 圆弧线条粗细
    float hitAreaSize  = 38.0f * dpi; // 点击区域大小

    // 窗口右下角位置
    float right  = windowPos.x + windowSize.x;
    float bottom = windowPos.y + windowSize.y;

    // 圆弧圆心（往内偏移，完全避开窗口圆角）
    float arcCenterX = right - windowRadius - arcInset - arcRadius;
    float arcCenterY = bottom - windowRadius - arcInset - arcRadius;

    // 点击检测区域（右下角方形区域）
    ImRect hitRect(right - hitAreaSize, bottom - hitAreaSize, right, bottom);

    bool isHovered = hitRect.Contains(mousePos) && IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // 处理拖动
    if (!state.isDragging) {
        if (isHovered && IsMouseClicked(0)) {
            state.isDragging      = true;
            state.dragStartMouseX = mousePos.x;
            state.dragStartMouseY = mousePos.y;
            state.dragStartSizeW  = windowSize.x;
            state.dragStartSizeH  = windowSize.y;
            SetActiveID(resizeId, window);
        }
    } else {
        if (IsMouseDown(0)) {
            float deltaX = mousePos.x - state.dragStartMouseX;
            float deltaY = mousePos.y - state.dragStartMouseY;

            float newSizeW = std::max(state.dragStartSizeW + deltaX, minWidth);
            float newSizeH = std::max(state.dragStartSizeH + deltaY, minHeight);

            SetWindowSize(ImVec2(newSizeW, newSizeH));
            SetActiveID(resizeId, window);
        } else {
            state.isDragging = false;
            if (GetActiveID() == resizeId) {
                ClearActiveID();
            }
        }
    }

    // 设置鼠标光标和 HoveredID
    if (isHovered || state.isDragging) {
        SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
        if (!state.isDragging) {
            SetHoveredID(resizeId);
        }
    }

    // 更新动画
    state.hoverState.target = (isHovered || state.isDragging) ? 1.0f : 0.0f;
    float hoverT            = std::clamp(state.hoverState.value, 0.0f, 1.0f);

    // 绘制圆弧
    ImDrawList* dl = GetWindowDrawList();

    // 圆弧颜色：默认 30% 透明度，悬停/拖动时 70%
    ImVec4 arcColor   = colors.onSurfaceVariant;
    arcColor.w        = 0.3f + 0.4f * hoverT;
    ImU32 arcColorU32 = ColorToU32(arcColor);

    // 绘制 90° 圆弧（从右边到下边）
    // PathArcTo 角度：0 = 右, PI/2 = 下, PI = 左, 3PI/2 = 上
    float arcStartAngle = 0.0f;         // 右边（3点钟方向）
    float arcEndAngle   = IM_PI * 0.5f; // 下边（6点钟方向）

    dl->PathArcTo(ImVec2(arcCenterX, arcCenterY), arcRadius, arcStartAngle, arcEndAngle,
                  12 // 分段数
    );
    dl->PathStroke(arcColorU32, false, arcThickness);

    // 在圆弧两端添加圆形端点（实现圆角效果）
    float capRadius = arcThickness * 0.5f;
    // 起点（右边，3点钟方向）
    ImVec2 startPoint(arcCenterX + arcRadius, arcCenterY);
    // 终点（下边，6点钟方向）
    ImVec2 endPoint(arcCenterX, arcCenterY + arcRadius);

    dl->AddCircleFilled(startPoint, capRadius, arcColorU32, 8);
    dl->AddCircleFilled(endPoint, capRadius, arcColorU32, 8);
}

} // namespace MD3
