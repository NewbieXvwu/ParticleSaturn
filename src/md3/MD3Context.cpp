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
    g_context.comboStates.clear();
    g_context.selectableStates.clear();
    g_context.collapsingHeaderStates.clear();
    g_context.windowStates.clear();
    g_context.scrollbarStates.clear();
    g_context.resizeStates.clear();
    g_context.smoothScrollStates.clear();

    g_context.initialized = false;
    std::cout << "[MD3] Material Design 3 UI system shutdown" << std::endl;
}

void BeginFrame(float dt) {
    g_context.deltaTime = dt;
    g_context.currentTime += dt;
    g_context.frameIndex++;

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

    int expectedFrameSeen = g_context.frameIndex - 1;
    for (auto& [id, state] : g_context.selectableStates) {
        if (state.lastFrameSeen != expectedFrameSeen) {
            state.hoverState.target = 0.0f;
        }
        state.hoverState.Update(dt);
    }

    for (auto& [id, state] : g_context.collapsingHeaderStates) {
        state.hoverState.Update(dt);
        state.openState.Update(dt);
        state.arrowRotation.Update(dt);
    }

    for (auto& [id, state] : g_context.windowStates) {
        state.closeButtonHover.Update(dt);
        state.closeButtonPress.Update(dt);
        // 窗口生命周期动画
        state.openProgress.Update(dt);
        state.scale.Update(dt);
        state.offsetY.Update(dt);
        state.alpha.Update(dt);
    }

    for (auto& [id, state] : g_context.scrollbarStates) {
        state.hoverState.Update(dt);
        state.dragState.Update(dt);
        state.visibility.Update(dt);
        // 更新隐藏计时器
        if (state.hideTimer > 0.0f) {
            state.hideTimer -= dt;
            if (state.hideTimer <= 0.0f) {
                state.visibility.target = 0.0f;
            }
        }
    }

    for (auto& [id, state] : g_context.resizeStates) {
        state.hoverState.Update(dt);
    }
}

void EndFrame() {
    // 预留给将来使用
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

struct RippleDrawData {
    float centerX;
    float centerY;
    float radius;
    float alpha;
    float boundsX;
    float boundsY;
    float boundsW;
    float boundsH;
    float cornerRadius;
    float colorR;
    float colorG;
    float colorB;
    float screenW;
    float screenH;
};

static void DrawRippleShaderCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const RippleDrawData* data = static_cast<const RippleDrawData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    if (!g_context.rippleProgram || !g_context.rippleVAO) {
        ImGui::MemFree(cmd->UserCallbackData);
        return;
    }

    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);

    glUseProgram(g_context.rippleProgram);

    const GLint uRippleCenter = glGetUniformLocation(g_context.rippleProgram, "uRippleCenter");
    const GLint uRippleRadius = glGetUniformLocation(g_context.rippleProgram, "uRippleRadius");
    const GLint uRippleAlpha = glGetUniformLocation(g_context.rippleProgram, "uRippleAlpha");
    const GLint uRippleColor = glGetUniformLocation(g_context.rippleProgram, "uRippleColor");
    const GLint uBounds = glGetUniformLocation(g_context.rippleProgram, "uBounds");
    const GLint uCornerRadius = glGetUniformLocation(g_context.rippleProgram, "uCornerRadius");
    const GLint uScreenSize = glGetUniformLocation(g_context.rippleProgram, "uScreenSize");

    if (uRippleCenter >= 0) glUniform2f(uRippleCenter, data->centerX, data->centerY);
    if (uRippleRadius >= 0) glUniform1f(uRippleRadius, data->radius);
    if (uRippleAlpha >= 0) glUniform1f(uRippleAlpha, data->alpha);
    if (uRippleColor >= 0) glUniform4f(uRippleColor, data->colorR, data->colorG, data->colorB, 1.0f);
    if (uBounds >= 0) glUniform4f(uBounds, data->boundsX, data->boundsY, data->boundsW, data->boundsH);
    if (uCornerRadius >= 0) glUniform1f(uCornerRadius, data->cornerRadius);
    if (uScreenSize >= 0) glUniform2f(uScreenSize, data->screenW, data->screenH);

    glBindVertexArray(g_context.rippleVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    glUseProgram(0);

    ImGui::MemFree(cmd->UserCallbackData);
}

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

    const ImGuiIO& io = ImGui::GetIO();
    const float    fbScaleX = (io.DisplayFramebufferScale.x > 0.0f) ? io.DisplayFramebufferScale.x : 1.0f;
    const float    fbScaleY = (io.DisplayFramebufferScale.y > 0.0f) ? io.DisplayFramebufferScale.y : 1.0f;
    const float    fbScaleR = std::max(fbScaleX, fbScaleY);

    // 获取当前窗口
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window) {
        return;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool useShader = (g_context.rippleProgram != 0 && g_context.rippleVAO != 0);

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

        // 保存裁剪区域（先用矩形裁剪把像素工作量限定在控件区域内）
        ImVec2 clipMin(currentBoundsX, currentBoundsY);
        ImVec2 clipMax(currentBoundsX + r.boundsW, currentBoundsY + r.boundsH);
        dl->PushClipRect(clipMin, clipMax, true);

        if (useShader) {
            RippleDrawData* data = static_cast<RippleDrawData*>(ImGui::MemAlloc(sizeof(RippleDrawData)));
            // Shader 使用 gl_FragCoord（Framebuffer 像素坐标），所以这里必须把 ImGui 坐标转换到 FB 像素坐标。
            data->centerX      = centerX * fbScaleX;
            data->centerY      = centerY * fbScaleY;
            data->radius       = r.radius * fbScaleR;
            data->alpha        = r.alpha;
            data->boundsX      = currentBoundsX * fbScaleX;
            data->boundsY      = currentBoundsY * fbScaleY;
            data->boundsW      = r.boundsW * fbScaleX;
            data->boundsH      = r.boundsH * fbScaleY;
            data->cornerRadius = r.cornerRadius * fbScaleR;
            data->colorR       = r.colorR;
            data->colorG       = r.colorG;
            data->colorB       = r.colorB;
            data->screenW      = io.DisplaySize.x * fbScaleX;
            data->screenH      = io.DisplaySize.y * fbScaleY;

            dl->AddCallback(DrawRippleShaderCallback, data);
            dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        } else {
            ImVec4 rippleColor(r.colorR, r.colorG, r.colorB, r.alpha);
            ImU32 col = ColorToU32(rippleColor);
            dl->AddCircleFilled(ImVec2(centerX, centerY), r.radius, col, 64);
        }

        dl->PopClipRect();
    }
}

//=============================================================================
// 工具函数实现
//=============================================================================

//=============================================================================
// 圆角裁剪（stencil）
//=============================================================================

struct RoundedClipBeginData {
    int prevRef;
    int newRef;
};

struct RoundedClipEndData {
    int ref;
    bool disable;
};

static std::vector<int> s_roundedClipStack;
static int              s_roundedClipRef = 0;

static void RoundedClipBeginCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const RoundedClipBeginData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    if (data->prevRef == 0) {
        glStencilFunc(GL_ALWAYS, data->newRef, 0xFF);
    } else {
        glStencilFunc(GL_EQUAL, data->prevRef, 0xFF);
    }

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    ImGui::MemFree(cmd->UserCallbackData);
}

static void RoundedClipEndCallback(const ImDrawList*, const ImDrawCmd* cmd) {
    const auto* data = static_cast<const RoundedClipEndData*>(cmd->UserCallbackData);
    if (!data) {
        return;
    }

    if (data->disable) {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFF);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        ImGui::MemFree(cmd->UserCallbackData);
        return;
    }

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0x00);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilFunc(GL_EQUAL, data->ref, 0xFF);

    ImGui::MemFree(cmd->UserCallbackData);
}

void PushRoundedClipRect(const ImVec2& clip_min, const ImVec2& clip_max, float rounding) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    int prevRef = s_roundedClipRef;
    int newRef  = prevRef + 1;
    if (newRef <= 0 || newRef > 255) {
        s_roundedClipRef = 0;
        s_roundedClipStack.clear();
        prevRef = 0;
        newRef  = 1;
    }

    s_roundedClipStack.push_back(prevRef);
    s_roundedClipRef = newRef;

    auto* begin = static_cast<RoundedClipBeginData*>(ImGui::MemAlloc(sizeof(RoundedClipBeginData)));
    begin->prevRef = prevRef;
    begin->newRef  = newRef;
    dl->AddCallback(RoundedClipBeginCallback, begin);

    ImVec4 dummy(1.0f, 1.0f, 1.0f, 1.0f);
    dl->AddRectFilled(clip_min, clip_max, ColorToU32(dummy), rounding);

    auto* end = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
    end->ref     = newRef;
    end->disable = false;
    dl->AddCallback(RoundedClipEndCallback, end);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

void PopRoundedClipRect() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl) {
        return;
    }

    if (s_roundedClipStack.empty()) {
        s_roundedClipRef = 0;
        auto* end        = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
        end->ref         = 0;
        end->disable     = true;
        dl->AddCallback(RoundedClipEndCallback, end);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        return;
    }

    int prevRef = s_roundedClipStack.back();
    s_roundedClipStack.pop_back();
    s_roundedClipRef = prevRef;

    auto* end    = static_cast<RoundedClipEndData*>(ImGui::MemAlloc(sizeof(RoundedClipEndData)));
    end->ref     = prevRef;
    end->disable = (prevRef == 0);
    dl->AddCallback(RoundedClipEndCallback, end);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

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

void AddImageRounded(ImDrawList* dl, unsigned int tex_id,
                     const ImVec2& p_min, const ImVec2& p_max,
                     const ImVec2& uv_min, const ImVec2& uv_max,
                     unsigned int col, float rounding, int flags) {
    if ((col & IM_COL32_A_MASK) == 0 || rounding < 0.5f) {
        // 无透明度或无圆角，直接使用普通 AddImage
        dl->AddImage((ImTextureID)(uintptr_t)tex_id, p_min, p_max, uv_min, uv_max, col);
        return;
    }

    // 创建圆角矩形路径
    dl->PathRect(p_min, p_max, rounding, flags);

    // 获取路径点数量
    int path_count = dl->_Path.Size;
    if (path_count < 3) {
        dl->PathClear();
        return;
    }

    // 计算尺寸用于 UV 映射
    float inv_w = 1.0f / (p_max.x - p_min.x);
    float inv_h = 1.0f / (p_max.y - p_min.y);
    float uv_w = uv_max.x - uv_min.x;
    float uv_h = uv_max.y - uv_min.y;

    // 切换到指定纹理
    dl->PushTextureID((ImTextureID)(uintptr_t)tex_id);

    // 预留顶点和索引空间（三角形扇形）
    int idx_count = (path_count - 2) * 3;
    dl->PrimReserve(idx_count, path_count);

    // 获取当前顶点索引基址
    ImDrawIdx idx_base = (ImDrawIdx)dl->_VtxCurrentIdx;

    // 添加顶点（带 UV 计算）
    for (int i = 0; i < path_count; i++) {
        ImVec2 p = dl->_Path[i];
        // 计算 UV：线性插值
        float u = uv_min.x + (p.x - p_min.x) * inv_w * uv_w;
        float v = uv_min.y + (p.y - p_min.y) * inv_h * uv_h;
        dl->PrimWriteVtx(p, ImVec2(u, v), col);
    }

    // 添加索引（三角形扇形）
    for (int i = 2; i < path_count; i++) {
        dl->PrimWriteIdx(idx_base);
        dl->PrimWriteIdx((ImDrawIdx)(idx_base + i - 1));
        dl->PrimWriteIdx((ImDrawIdx)(idx_base + i));
    }

    dl->PopTextureID();
    dl->PathClear();
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
