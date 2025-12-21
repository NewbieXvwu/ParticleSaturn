// MD3Context.cpp - MD3 全局状态、初始化和帧管理

#include <glad/glad.h>

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <iostream>
#include <unordered_map>
#include <vector>

#include "MD3.h"
#include "MD3Shaders.h"

namespace MD3 {

// 全局上下文单例
static MD3Context g_context;

MD3Context& GetContext() {
    return g_context;
}

// 编译着色器
static GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        std::cerr << "[MD3] Shader compilation failed: " << infoLog << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// 创建着色器程序
static GLuint CreateProgram(const char* vertexSrc, const char* fragmentSrc) {
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertexSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragmentSrc);

    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        std::cerr << "[MD3] Program linking failed: " << infoLog << std::endl;
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return program;
}

void Init(float dpiScale) {
    if (g_context.initialized) {
        return;
    }

    g_context.dpiScale   = dpiScale;
    g_context.isDarkMode = true;
    g_context.colors     = GetDarkColorScheme();

    // 创建 Ripple 着色器程序
    g_context.rippleProgram = CreateProgram(MD3Shaders::VertexRipple, MD3Shaders::FragmentRipple);

    if (!g_context.rippleProgram) {
        std::cerr << "[MD3] Failed to create ripple shader program" << std::endl;
    }

    // 创建全屏四边形 VAO/VBO
    float quadVerts[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};

    glGenVertexArrays(1, &g_context.rippleVAO);
    glGenBuffers(1, &g_context.rippleVBO);

    glBindVertexArray(g_context.rippleVAO);
    glBindBuffer(GL_ARRAY_BUFFER, g_context.rippleVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glBindVertexArray(0);

    g_context.initialized = true;
    std::cout << "[MD3] Material Design 3 UI system initialized" << std::endl;
}

void Shutdown() {
    if (!g_context.initialized) {
        return;
    }

    if (g_context.rippleProgram) {
        glDeleteProgram(g_context.rippleProgram);
        g_context.rippleProgram = 0;
    }

    if (g_context.rippleVAO) {
        glDeleteVertexArrays(1, &g_context.rippleVAO);
        g_context.rippleVAO = 0;
    }

    if (g_context.rippleVBO) {
        glDeleteBuffers(1, &g_context.rippleVBO);
        g_context.rippleVBO = 0;
    }

    g_context.ripples.clear();
    g_context.toggleStates.clear();
    g_context.buttonStates.clear();
    g_context.sliderStates.clear();
    g_context.cardStates.clear();

    g_context.initialized = false;
    std::cout << "[MD3] Material Design 3 UI system shutdown" << std::endl;
}

void BeginFrame(float dt) {
    g_context.deltaTime = dt;
    g_context.currentTime += dt;

    // 更新所有 Ripple 动画
    auto&       ripples = g_context.ripples;
    const auto& config  = g_context.rippleConfig;

    for (auto it = ripples.begin(); it != ripples.end();) {
        RippleState& r = *it;
        r.time += dt;

        if (!r.fadeOut) {
            // 扩散阶段
            float expandProgress = r.time / config.expandDuration;
            if (expandProgress >= 1.0f) {
                expandProgress = 1.0f;
                r.fadeOut      = true;
                r.time         = 0.0f; // 重置时间用于淡出
            }

            // 使用 ease-out 曲线
            float eased = 1.0f - (1.0f - expandProgress) * (1.0f - expandProgress);
            r.radius    = r.maxRadius * eased;
            r.alpha     = config.maxAlpha;
        } else {
            // 淡出阶段
            float fadeProgress = r.time / config.fadeDuration;
            if (fadeProgress >= 1.0f) {
                it = ripples.erase(it);
                continue;
            }

            r.radius = r.maxRadius;
            r.alpha  = config.maxAlpha * (1.0f - fadeProgress);
        }

        ++it;
    }

    // 更新所有控件动画状态
    for (auto& [id, state] : g_context.toggleStates) {
        state.knobPosition.Update(dt);
        state.trackFill.Update(dt);
        state.knobScale.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.buttonStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
        state.pressState.Update(dt);
    }

    for (auto& [id, state] : g_context.sliderStates) {
        state.thumbScale.Update(dt);
        state.activeTrack.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.cardStates) {
        state.elevation.Update(dt);
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.comboStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    for (auto& [id, state] : g_context.collapsingHeaderStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }
}

void EndFrame() {
    // Ripples 现在在 DrawRipples() 中使用 ImDrawList 渲染
    // 这里不再需要做任何事情
}

void SetDarkMode(bool dark) {
    if (g_context.isDarkMode == dark) {
        return;
    }

    g_context.isDarkMode = dark;
    g_context.colors     = dark ? GetDarkColorScheme() : GetLightColorScheme();

    std::cout << "[MD3] Theme changed to: " << (dark ? "Dark" : "Light") << std::endl;
}

bool IsDarkMode() {
    return g_context.isDarkMode;
}

void SetScreenSize(float width, float height) {
    g_context.screenWidth  = width;
    g_context.screenHeight = height;
}

void SetDpiScale(float scale) {
    g_context.dpiScale = scale;
}

//=============================================================================
// Ripple API 实现
//=============================================================================

void TriggerRipple(ImGuiID id, float centerX, float centerY, float boundsX, float boundsY, float boundsW, float boundsH,
                   float cornerRadius) {
    // 计算最大半径（覆盖整个控件对角线）
    float dx1 = centerX - boundsX;
    float dx2 = (boundsX + boundsW) - centerX;
    float dy1 = centerY - boundsY;
    float dy2 = (boundsY + boundsH) - centerY;

    float maxDx     = std::max(dx1, dx2);
    float maxDy     = std::max(dy1, dy2);
    float maxRadius = std::sqrt(maxDx * maxDx + maxDy * maxDy);

    // 获取 Ripple 颜色
    const auto& colors      = g_context.colors;
    ImVec4      rippleColor = g_context.isDarkMode ? colors.onSurface : colors.primary;

    // 获取当前窗口信息
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    RippleState state;
    state.widgetId = id;
    // 存储相对于控件的点击位置
    state.relCenterX   = centerX - boundsX;
    state.relCenterY   = centerY - boundsY;
    state.radius       = 0.0f;
    state.maxRadius    = maxRadius;
    state.alpha        = 0.0f;
    state.time         = 0.0f;
    state.boundsW      = boundsW;
    state.boundsH      = boundsH;
    state.cornerRadius = cornerRadius;
    state.colorR       = rippleColor.x;
    state.colorG       = rippleColor.y;
    state.colorB       = rippleColor.z;
    state.colorA       = 1.0f;
    // 存储窗口信息用于滚动补偿
    if (window) {
        state.windowId          = window->ID;
        state.initialWindowPosX = window->Pos.x;
        state.initialWindowPosY = window->Pos.y;
        state.initialScrollX    = window->Scroll.x;
        state.initialScrollY    = window->Scroll.y;
    }
    state.initialBoundsX = boundsX;
    state.initialBoundsY = boundsY;
    state.active         = true;
    state.fadeOut        = false;

    g_context.ripples.push_back(state);
}

void TriggerRippleForCurrentItem(ImGuiID id, float cornerRadius) {
    ImVec2 mousePos = ImGui::GetIO().MousePos;
    ImVec2 itemMin  = ImGui::GetItemRectMin();
    ImVec2 itemMax  = ImGui::GetItemRectMax();

    TriggerRipple(id, mousePos.x, mousePos.y, itemMin.x, itemMin.y, itemMax.x - itemMin.x, itemMax.y - itemMin.y,
                  cornerRadius);
}

void DrawRipples() {
    if (g_context.ripples.empty()) {
        return;
    }

    // 获取当前窗口
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) {
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 绘制属于当前窗口的所有 Ripple
    for (const auto& r : g_context.ripples) {
        if (r.alpha <= 0.001f) {
            continue;
        }
        if (r.windowId != window->ID) {
            continue; // 只绘制当前窗口的 ripple
        }

        // 计算滚动偏移量
        float scrollDeltaX = window->Scroll.x - r.initialScrollX;
        float scrollDeltaY = window->Scroll.y - r.initialScrollY;

        // 计算当前控件位置（补偿滚动）
        float currentBoundsX = r.initialBoundsX - scrollDeltaX;
        float currentBoundsY = r.initialBoundsY - scrollDeltaY;

        // 计算 ripple 中心的屏幕位置
        float centerX = currentBoundsX + r.relCenterX;
        float centerY = currentBoundsY + r.relCenterY;

        // Ripple 颜色
        ImVec4 rippleColor(r.colorR, r.colorG, r.colorB, r.alpha);
        ImU32  col = ColorToU32(rippleColor);

        // 保存裁剪区域
        ImVec2 clipMin(currentBoundsX, currentBoundsY);
        ImVec2 clipMax(currentBoundsX + r.boundsW, currentBoundsY + r.boundsH);
        dl->PushClipRect(clipMin, clipMax, true);

        // 使用多个同心圆来模拟 ripple 效果（渐变边缘）
        // 简化版本：只画一个实心圆
        int segments = 64;
        dl->AddCircleFilled(ImVec2(centerX, centerY), r.radius, col, segments);

        dl->PopClipRect();
    }
}

//=============================================================================
// 工具函数实现
//=============================================================================

ImVec4 BlendColors(const ImVec4& base, const ImVec4& overlay, float alpha) {
    return ImVec4(base.x + (overlay.x - base.x) * alpha, base.y + (overlay.y - base.y) * alpha,
                  base.z + (overlay.z - base.z) * alpha, base.w + (overlay.w - base.w) * alpha);
}

ImVec4 ApplyStateLayer(const ImVec4& base, const ImVec4& stateColor, float stateAlpha) {
    // 正确的状态层混合：在基础颜色上叠加半透明状态颜色
    return ImVec4(base.x * (1.0f - stateAlpha) + stateColor.x * stateAlpha,
                  base.y * (1.0f - stateAlpha) + stateColor.y * stateAlpha,
                  base.z * (1.0f - stateAlpha) + stateColor.z * stateAlpha, base.w);
}

unsigned int ColorToU32(const ImVec4& color) {
    unsigned int r = (unsigned int)(color.x * 255.0f);
    unsigned int g = (unsigned int)(color.y * 255.0f);
    unsigned int b = (unsigned int)(color.z * 255.0f);
    unsigned int a = (unsigned int)(color.w * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

ImVec4 HexToColor(unsigned int hex, float alpha) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha);
}

} // namespace MD3

//=============================================================================
// ImGui 集成钩子实现
//=============================================================================

#ifdef IMGUI_MD3_ENABLED

extern "C" void MD3_OnNewFrame(float dt) {
    MD3::BeginFrame(dt);
}

extern "C" void MD3_TriggerRipple(unsigned int id, float mouseX, float mouseY, float bbMinX, float bbMinY, float bbMaxX,
                                  float bbMaxY) {
    float cornerRadius = ImGui::GetStyle().FrameRounding;
    MD3::TriggerRipple(id, mouseX, mouseY, bbMinX, bbMinY, bbMaxX - bbMinX, bbMaxY - bbMinY, cornerRadius);
}

extern "C" bool MD3_Checkbox(const char* label, bool* v) {
    return MD3::Toggle(label, v);
}

#endif
