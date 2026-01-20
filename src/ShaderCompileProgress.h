#pragma once
// ShaderCompileProgress.h - 着色器编译进度条 UI
// 使用 ImGui 渲染 MD3 风格的进度条，带弹簧动画

#include <atomic>
#include <cmath>
#include <cstdint>

#include "imgui.h"

namespace ShaderCompileProgress {

// 弹簧动画状态
struct SpringAnimator {
    float position = 0.0f;
    float velocity = 0.0f;

    // 高阻尼弹簧参数（消除过冲）
    static constexpr float kStiffness = 180.0f;
    static constexpr float kDamping   = 28.0f; // 高阻尼，临界阻尼约为 2*sqrt(stiffness) ≈ 26.8

    void Update(float target, float dt) {
        float force = (target - position) * kStiffness - velocity * kDamping;
        velocity += force * dt;
        position += velocity * dt;

        // 接近目标时吸附，避免无限逼近
        if (fabsf(position - target) < 0.001f && fabsf(velocity) < 0.01f) {
            position = target;
            velocity = 0.0f;
        }
    }

    void Reset() {
        position = 0.0f;
        velocity = 0.0f;
    }
};

// 进度条渲染器
class ProgressRenderer {
  public:
    ProgressRenderer() = default;

    // 设置总步数
    void SetTotal(int total) {
        total_     = total;
        completed_ = 0;
        animator_.Reset();
    }

    // 增加完成计数（线程安全）
    void IncrementCompleted() { completed_.fetch_add(1, std::memory_order_relaxed); }

    // 获取当前完成数
    int GetCompleted() const { return completed_.load(std::memory_order_relaxed); }

    // 是否全部完成
    bool IsComplete() const { return GetCompleted() >= total_; }

    // 渲染进度条
    // @param isDarkMode 是否深色模式
    // @param dt 帧时间（秒）
    void Render(bool isDarkMode, float dt) {
        // 更新动画
        float targetProgress = (total_ > 0) ? static_cast<float>(GetCompleted()) / static_cast<float>(total_) : 0.0f;
        animator_.Update(targetProgress, dt);

        // 获取窗口尺寸
        ImGuiIO& io           = ImGui::GetIO();
        float    windowWidth  = io.DisplaySize.x;
        float    windowHeight = io.DisplaySize.y;

        // MD3 颜色
        ImU32 bgColor, trackColor, fillColor, textColor;
        if (isDarkMode) {
            bgColor    = IM_COL32(0x12, 0x12, 0x14, 255); // surface dark: #121214
            trackColor = IM_COL32(0x43, 0x47, 0x4E, 255); // surfaceVariant dark: #43474E
            fillColor  = IM_COL32(0xA6, 0xCF, 0xFF, 255); // primary dark: #A6CFFF
            textColor  = IM_COL32(0xE2, 0xE2, 0xE5, 255); // onSurface dark: #E2E2E5
        } else {
            bgColor    = IM_COL32(0xF9, 0xF9, 0xFC, 255); // surface light: #F9F9FC
            trackColor = IM_COL32(0xDF, 0xE2, 0xEB, 255); // surfaceVariant light: #DFE2EB
            fillColor  = IM_COL32(0x00, 0x59, 0xA6, 255); // primary light: #0059A6
            textColor  = IM_COL32(0x1A, 0x1C, 0x1E, 255); // onSurface light: #1A1C1E
        }

        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        // 绘制背景
        drawList->AddRectFilled(ImVec2(0, 0), ImVec2(windowWidth, windowHeight), bgColor);

        // 进度条尺寸
        float barWidth  = 280.0f;
        float barHeight = 6.0f;
        float barX      = (windowWidth - barWidth) / 2.0f;
        float barY      = windowHeight / 2.0f;
        float rounding  = barHeight / 2.0f; // 药丸形状

        // 绘制轨道
        drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barWidth, barY + barHeight), trackColor, rounding);

        // 绘制填充
        float fillWidth = barWidth * animator_.position;
        if (fillWidth > 0.1f) {
            drawList->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + fillWidth, barY + barHeight), fillColor, rounding);
        }

        // 绘制文字 "正在编译着色器..."
        const char* titleText = reinterpret_cast<const char*>(u8"正在编译着色器...");
        ImVec2      titleSize = ImGui::CalcTextSize(titleText);
        float       titleX    = (windowWidth - titleSize.x) / 2.0f;
        float       titleY    = barY - titleSize.y - 16.0f;
        drawList->AddText(ImVec2(titleX, titleY), textColor, titleText);

        // 绘制计数 "5 / 7"
        char   countText[32];
        snprintf(countText, sizeof(countText), "%d / %d", GetCompleted(), total_);
        ImVec2 countSize = ImGui::CalcTextSize(countText);
        float  countX    = (windowWidth - countSize.x) / 2.0f;
        float  countY    = barY + barHeight + 12.0f;
        drawList->AddText(ImVec2(countX, countY), textColor, countText);
    }

  private:
    int              total_     = 0;
    std::atomic<int> completed_ = 0;
    SpringAnimator   animator_;
};

} // namespace ShaderCompileProgress
