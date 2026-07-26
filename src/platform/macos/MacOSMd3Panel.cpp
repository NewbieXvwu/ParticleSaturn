#include "MacOSMd3Panel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "imgui.h"

#include "MD3.h"
#include "MD3Log.h"
#include "app/AppController.h"
#include "services/diagnostics/DiagnosticBus.h"

namespace ParticleSaturn::Platform::MacOS {
namespace {

template <class Command>
void DispatchAndSave(ParticleSaturn::App::AppController& controller, const Command& command,
                     const Md3PanelCallbacks& callbacks) {
    controller.Dispatch(command);
    if (callbacks.save) callbacks.save();
}

const char* MaterialLabel(ParticleSaturn::App::WindowMaterial material) {
    constexpr const char* labels[] = {"Solid", "Transparent", "System blur", "App Acrylic"};
    return labels[static_cast<unsigned int>(material)];
}

const char* ApiLabel(ParticleSaturn::App::GraphicsApi api) {
    constexpr const char* labels[] = {"OpenGL 4.1", "Vulkan", "Metal"};
    return labels[static_cast<unsigned int>(api)];
}

float EaseOutCubic(float t) {
    const float inverted = 1.0f - t;
    return 1.0f - inverted * inverted * inverted;
}

// Catmull-Rom 样条插值：t=0 返回 p1，t=1 返回 p2。
float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

// FPS 显示历史：低频采样（50ms 一个样本）+ 滚动动画，行为与旧
// OpenGL 版 RingBufferFPS 的显示部分一致。
class FpsHistoryTracker {
public:
    static constexpr int HistorySize = 60;
    static constexpr float SampleIntervalSeconds = 0.05f;

    FpsHistoryTracker() {
        for (float& sample : history_) sample = 60.0f;
    }

    void AddFrame(float deltaSeconds) {
        accumTime_ += deltaSeconds;
        accumFps_ += deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 60.0f;
        ++accumCount_;
        if (accumTime_ >= SampleIntervalSeconds) {
            history_[index_] = accumFps_ / static_cast<float>(accumCount_);
            index_ = (index_ + 1) % HistorySize;
            accumTime_ -= SampleIntervalSeconds;
            accumFps_ = 0.0f;
            accumCount_ = 0;
            // 重置滚动动画，保留本帧多出的进度，确保匀速滚动。
            scrollAnimTime_ = accumTime_;
        } else {
            scrollAnimTime_ += deltaSeconds;
        }
    }

    // logicalIndex: 0 = 最旧，HistorySize-1 = 最新。
    float Value(int logicalIndex) const { return history_[(index_ + logicalIndex) % HistorySize]; }

    // 0 = 刚采样，1 = 即将采样；ease-out 让曲线滚动开始快结束慢。
    float ScrollProgress() const {
        const float t = scrollAnimTime_ / SampleIntervalSeconds;
        return EaseOutCubic(t < 1.0f ? t : 1.0f);
    }

private:
    float history_[HistorySize];
    int index_ = 0;
    float accumTime_ = 0.0f;
    float accumFps_ = 0.0f;
    int accumCount_ = 0;
    float scrollAnimTime_ = 0.0f;
};

void DrawFpsHistoryGraph(FpsHistoryTracker& fpsHistory, const ParticleSaturn::App::AppState& state,
                         const Md3PanelCallbacks& callbacks, float dpi) {
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    constexpr int historySize = FpsHistoryTracker::HistorySize;
    const float scrollProgress = fpsHistory.ScrollProgress();

    // 目标 Y 轴范围：根据当前数据自适应。
    float dataMin = fpsHistory.Value(0);
    float dataMax = dataMin;
    for (int i = 1; i < historySize; ++i) {
        const float value = fpsHistory.Value(i);
        dataMin = std::min(dataMin, value);
        dataMax = std::max(dataMax, value);
    }

    // 最小显示范围，防止帧率稳定时过度放大。
    constexpr float MinDisplayRange = 30.0f;
    if (dataMax - dataMin < MinDisplayRange) {
        const float center = (dataMax + dataMin) * 0.5f;
        dataMin = center - MinDisplayRange * 0.5f;
        dataMax = center + MinDisplayRange * 0.5f;
    }

    const float margin = (dataMax - dataMin) * 0.1f;
    const float targetMinValue = std::max(0.0f, dataMin - margin);
    const float targetMaxValue = dataMax + margin;

    // Y 轴范围平滑动画。
    static float animMinValue = 60.0f;
    static float animMaxValue = 120.0f;
    constexpr float animSpeed = 8.0f;
    const float deltaTime = ImGui::GetIO().DeltaTime;
    animMinValue += (targetMinValue - animMinValue) * (1.0f - std::exp(-animSpeed * deltaTime));
    animMaxValue += (targetMaxValue - animMaxValue) * (1.0f - std::exp(-animSpeed * deltaTime));
    if (std::abs(targetMinValue - animMinValue) < 0.1f) animMinValue = targetMinValue;
    if (std::abs(targetMaxValue - animMaxValue) < 0.1f) animMaxValue = targetMaxValue;

    const float minValue = animMinValue;
    const float maxValue = animMaxValue;
    const float valueRange = std::max(1.0f, maxValue - minValue);

    const ImVec2 plotSize(ImGui::GetContentRegionAvail().x, 50.0f);
    const ImVec2 plotPos = ImGui::GetCursorScreenPos();
    const ImVec2 plotEnd(plotPos.x + plotSize.x, plotPos.y + plotSize.y);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    const auto& md3Context = MD3::GetContext();
    const float cornerRadius = 6.0f * dpi;
    const ImU32 lineColor = ImGui::GetColorU32(ImVec4(0.4f, 0.8f, 1.0f, 1.0f));
    const ImU32 axisColor = ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.6f, 0.9f));
    const ImU32 borderColor = md3Context.isDarkMode ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 20);

    // 背景：优先 1/12 弱模糊 Acrylic（OpenGL 着色器路径），其次后端回调
    // （Metal/Vulkan 的合成 Acrylic），否则退回纯色。
    if (md3Context.blurEnabled && md3Context.blurTextureID2 != 0 && md3Context.screenWidth > 0.0f &&
        md3Context.screenHeight > 0.0f) {
        const ImVec2 uv0(plotPos.x / md3Context.screenWidth, 1.0f - plotPos.y / md3Context.screenHeight);
        const ImVec2 uv1(plotEnd.x / md3Context.screenWidth, 1.0f - plotEnd.y / md3Context.screenHeight);
        MD3::AddImageRounded(drawList, md3Context.blurTextureID2, plotPos, plotEnd, uv0, uv1,
                             IM_COL32(255, 255, 255, 255), cornerRadius);
        if (md3Context.noiseTextureID != 0 && md3Context.noiseIntensity > 0.0f) {
            const float intensity = std::clamp(md3Context.noiseIntensity, 0.0f, 0.1f);
            const int alpha = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
            MD3::AddImageRounded(drawList, md3Context.noiseTextureID, plotPos, plotEnd, uv0, uv1,
                                 IM_COL32(255, 255, 255, alpha), cornerRadius);
        }
        drawList->AddRect(plotPos, plotEnd, borderColor, cornerRadius, 0, 1.0f);
    } else if (state.ui.blurEnabled && callbacks.drawGraphAcrylic) {
        callbacks.drawGraphAcrylic(drawList, plotPos, plotSize, cornerRadius);
        drawList->AddRect(plotPos, plotEnd, borderColor, cornerRadius, 0, 1.0f);
    } else {
        drawList->AddRectFilled(plotPos, plotEnd, MD3::ColorToU32(md3Context.colors.surfaceContainerHigh),
                                cornerRadius);
    }

    drawList->PushClipRect(plotPos, plotEnd, true);

    const auto toScreen = [&](float logicalX, float value) {
        const float adjustedX = logicalX - scrollProgress;
        const float x = plotPos.x + (adjustedX / static_cast<float>(historySize - 1)) * plotSize.x;
        const float clampedValue = std::clamp(value, minValue, maxValue);
        const float y = plotPos.y + plotSize.y - ((clampedValue - minValue) / valueRange) * plotSize.y;
        return ImVec2(x, y);
    };

    // Catmull-Rom 平滑曲线。
    constexpr int subdivisions = 4;
    ImVector<ImVec2> points;
    points.reserve((historySize - 1) * subdivisions + 1);
    for (int i = 0; i < historySize - 1; ++i) {
        const float p0 = fpsHistory.Value(i > 0 ? i - 1 : 0);
        const float p1 = fpsHistory.Value(i);
        const float p2 = fpsHistory.Value(i + 1);
        const float p3 = fpsHistory.Value(i + 2 < historySize ? i + 2 : historySize - 1);
        for (int j = 0; j < subdivisions; ++j) {
            const float t = static_cast<float>(j) / subdivisions;
            const ImVec2 point = toScreen(static_cast<float>(i) + t, CatmullRom(p0, p1, p2, p3, t));
            if (point.x >= plotPos.x - 5.0f && point.x <= plotEnd.x + 5.0f) points.push_back(point);
        }
    }
    const ImVec2 lastPoint = toScreen(static_cast<float>(historySize - 1), fpsHistory.Value(historySize - 1));
    if (lastPoint.x >= plotPos.x - 5.0f && lastPoint.x <= plotEnd.x + 5.0f) points.push_back(lastPoint);
    if (points.Size >= 2) drawList->AddPolyline(points.Data, points.Size, lineColor, ImDrawFlags_None, 1.5f);

    drawList->PopClipRect();

    // Y 轴刻度 overlay（绘制在曲线之上）。
    char maxLabel[16];
    char minLabel[16];
    std::snprintf(maxLabel, sizeof(maxLabel), "%.0f", maxValue);
    std::snprintf(minLabel, sizeof(minLabel), "%.0f", minValue);
    drawList->AddText(ImVec2(plotPos.x + 3.0f, plotPos.y + 1.0f), axisColor, maxLabel);
    drawList->AddText(ImVec2(plotPos.x + 3.0f, plotEnd.y - 13.0f), axisColor, minLabel);

    ImGui::Dummy(plotSize);
    if (ImGui::IsItemHovered()) {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const float relX = (mousePos.x - plotPos.x) / plotSize.x;
        const int index = static_cast<int>(relX * static_cast<float>(historySize - 1) + scrollProgress + 0.5f);
        if (index >= 0 && index < historySize) {
            ImGui::BeginTooltip();
            ImGui::Text("%.0f FPS", fpsHistory.Value(index));
            ImGui::EndTooltip();
        }
    }
}

void LabeledRow(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s", label);
    ImGui::TableNextColumn();
    va_list args;
    va_start(args, fmt);
    char buffer[128];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    ImGui::TextUnformatted(buffer);
}

// 把 DiagnosticBus 的新记录并入调试日志，确保诊断在 Log 节可见。
void SyncDiagnosticsToLog() {
    static std::chrono::system_clock::time_point lastSeen{};
    const auto records = ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().SnapshotSince(lastSeen);
    for (const auto& record : records) {
        const auto level = record.severity == ParticleSaturn::Services::Diagnostics::Severity::Error
            ? MD3::LogLevel::Error
            : record.severity == ParticleSaturn::Services::Diagnostics::Severity::Warning ? MD3::LogLevel::Warn
                                                                                          : MD3::LogLevel::Info;
        MD3::DebugLog::Instance().Add(level, "[" + record.domain + "] " + record.code + ": " + record.message);
    }
}

// 暂停/继续按钮：完整复刻旧 UI 的自绘 MD3 按钮（阴影 + 状态层 + ripple +
// tooltip），图标改为矢量绘制以摆脱旧版的 OpenGL 纹理烘焙依赖。
void DrawLogPauseButton(float buttonSize, float dpi) {
    const bool isPaused = MD3::DebugLog::Instance().IsPaused();
    ImGui::PushID("LogPauseBtn");
    ImGui::InvisibleButton("##btn", ImVec2(buttonSize, buttonSize));

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 btnMin = ImGui::GetItemRectMin();
    const ImVec2 btnMax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool held = ImGui::IsItemActive();
    const bool clicked = ImGui::IsItemClicked(0);

    const MD3::MD3ColorScheme colors = MD3::IsDarkMode() ? MD3::GetDarkColorScheme() : MD3::GetLightColorScheme();
    const float rounding = std::min(12.0f * dpi, buttonSize * 0.5f);

    if (clicked) {
        MD3::TriggerRippleForCurrentItem(ImGui::GetItemID(), rounding);
        MD3::DebugLog::Instance().SetPaused(!isPaused);
    }
    if (hovered) {
        ImGui::SetTooltip("%s", isPaused ? "Resume log" : "Pause log");
    }

    // 轻微阴影（更接近 MD3 elevation）
    {
        ImVec4 shadow = colors.shadow;
        shadow.w = 0.18f;
        const ImVec2 shadowMin(btnMin.x, btnMin.y + 1.0f * dpi);
        const ImVec2 shadowMax(btnMax.x, btnMax.y + 1.0f * dpi);
        drawList->AddRectFilled(shadowMin, shadowMax, MD3::ColorToU32(shadow), rounding);
    }

    // 底色 + 状态层
    ImVec4 bgColor = colors.surfaceContainerHigh;
    if (hovered || held) {
        const float alpha = held ? colors.stateLayerPressed : colors.stateLayerHover;
        bgColor = MD3::ApplyStateLayer(bgColor, colors.onSurface, alpha);
    }
    drawList->AddRectFilled(btnMin, btnMax, MD3::ColorToU32(bgColor), rounding);

    // 图标（矢量绘制）：未暂停显示“暂停”双竖条，已暂停显示“播放”三角。
    const float centerX = (btnMin.x + btnMax.x) * 0.5f;
    const float centerY = (btnMin.y + btnMax.y) * 0.5f;
    const float iconHalf = 7.0f * dpi;
    const ImU32 iconColor = MD3::ColorToU32(colors.onSurface);
    if (isPaused) {
        drawList->AddTriangleFilled(ImVec2(centerX - iconHalf * 0.6f, centerY - iconHalf),
                                    ImVec2(centerX - iconHalf * 0.6f, centerY + iconHalf),
                                    ImVec2(centerX + iconHalf, centerY), iconColor);
    } else {
        const float barWidth = iconHalf * 0.45f;
        const float barGap = iconHalf * 0.35f;
        drawList->AddRectFilled(ImVec2(centerX - barGap - barWidth, centerY - iconHalf),
                                ImVec2(centerX - barGap, centerY + iconHalf), iconColor, barWidth * 0.4f);
        drawList->AddRectFilled(ImVec2(centerX + barGap, centerY - iconHalf),
                                ImVec2(centerX + barGap + barWidth, centerY + iconHalf), iconColor, barWidth * 0.4f);
    }
    ImGui::PopID();
}

void RenderLogSection(float dpi) {
    static char logSearchBuffer[128] = "";
    static int logLevelFilter = 0; // 0=全部, 1=Info, 2=Warn, 3=Error

    // 第一行：级别过滤、搜索和暂停按钮
    const float controlHeight = 40.0f * dpi; // MD3 标准控件高度（与 MD3::Combo 一致）
    const float buttonSize = controlHeight;  // 暂停按钮尺寸（正方形）

    ImGui::SetNextItemWidth(80.0f * dpi);
    constexpr const char* levelLabels[] = {"All", "Info", "Warn", "Error"};
    MD3::Combo("##LogLevel", &logLevelFilter, levelLabels, IM_ARRAYSIZE(levelLabels));

    ImGui::SameLine();
    // 搜索栏宽度 = 可用宽度 - 暂停按钮 - 间距 - 右侧留白
    const float itemSpacingX = ImGui::GetStyle().ItemSpacing.x;
    const float rightMargin = 12.0f * dpi; // 让暂停按钮不要贴右边界
    const float minSearchWidth = 120.0f * dpi;
    const float contentAvailX = ImGui::GetContentRegionAvail().x;

    float searchWidth = contentAvailX - buttonSize - itemSpacingX - rightMargin;
    if (searchWidth < minSearchWidth) {
        // 空间不足时优先保证输入框可用：取消右侧留白
        searchWidth = contentAvailX - buttonSize - itemSpacingX;
    }
    searchWidth = std::max(searchWidth, 1.0f);

    ImGui::SetNextItemWidth(searchWidth);

    // InputText 走 ImGui 自带绘制：通过 FramePadding 精确对齐 MD3 的 40dp
    // 高度，并用圆角裁剪避免文字“顶出”圆角区域
    const float padY = std::max(0.0f, (controlHeight - ImGui::GetFontSize()) * 0.5f);
    const float inputRounding = controlHeight * 0.5f;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f * dpi, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, inputRounding);

    const ImVec2 inputPos = ImGui::GetCursorScreenPos();
    const ImVec2 inputSize(searchWidth, controlHeight);
    MD3::PushRoundedClipRect(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y), inputRounding);
    ImGui::InputTextWithHint("##LogSearch", "Search", logSearchBuffer, sizeof(logSearchBuffer));
    MD3::PopRoundedClipRect();

    ImGui::PopStyleVar(2);

    // 右键粘贴菜单
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginPopupContextItem("##LogSearchContext")) {
        const char* clipText = ImGui::GetClipboardText();
        const bool canPaste = clipText != nullptr && clipText[0] != '\0';
        if (MD3::MenuItem("Paste", canPaste, 36.0f * dpi)) {
            // 追加粘贴内容到搜索栏
            const size_t currentLen = std::strlen(logSearchBuffer);
            size_t clipLen = std::strlen(clipText);
            const size_t maxAppend = sizeof(logSearchBuffer) - 1 - currentLen;
            clipLen = std::min(clipLen, maxAppend);
            if (clipLen > 0) {
                std::memcpy(logSearchBuffer + currentLen, clipText, clipLen);
                logSearchBuffer[currentLen + clipLen] = '\0';
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);

    // 暂停/继续按钮
    ImGui::SameLine();
    DrawLogPauseButton(buttonSize, dpi);

    // 第二行：清空和复制按钮
    if (MD3::TonalButton("Clear log")) {
        MD3::DebugLog::Instance().Clear();
    }
    ImGui::SameLine();
    if (MD3::TonalButton("Copy all")) {
        const std::string filteredText = MD3::DebugLog::Instance().GetFilteredText(logSearchBuffer, logLevelFilter);
        ImGui::SetClipboardText(filteredText.c_str());
    }

    // 日志列表（带过滤和搜索）
    MD3::DebugLog::Instance().Draw(logSearchBuffer, logLevelFilter);
}

void RenderHandTrackingStatusCard(const Md3PanelHandTrackingStatus& handStatus, float dpi) {
    const char* statusText = "Unavailable";
    ImVec4 statusColor(0.6f, 0.6f, 0.6f, 1.0f);
    switch (handStatus.tracker) {
    case Md3PanelHandTrackingStatus::Tracker::Initializing:
        statusText = "Initializing";
        statusColor = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
        break;
    case Md3PanelHandTrackingStatus::Tracker::Ready:
        statusText = "Ready";
        statusColor = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
        break;
    case Md3PanelHandTrackingStatus::Tracker::Failed:
        statusText = "Failed";
        statusColor = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        break;
    case Md3PanelHandTrackingStatus::Tracker::Unavailable:
        break;
    }

    if (ImGui::BeginTable("TrackerStatusTable", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f * dpi);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Tracker");
        ImGui::TableNextColumn();
        ImGui::TextColored(statusColor, "%s", statusText);

        if (handStatus.tracker == Md3PanelHandTrackingStatus::Tracker::Failed && !handStatus.errorMessage.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Error");
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
            ImGui::TextWrapped("%s", handStatus.errorMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Camera");
        ImGui::TableNextColumn();
        if (handStatus.tracker == Md3PanelHandTrackingStatus::Tracker::Ready && !handStatus.cameraInfo.empty()) {
            ImGui::TextUnformatted(handStatus.cameraInfo.c_str());
        } else {
            ImGui::TextDisabled("N/A");
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Hand detected");
        ImGui::TableNextColumn();
        if (handStatus.handDetected) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Yes");
        } else {
            ImGui::TextUnformatted("No");
        }

        ImGui::EndTable();
    }
}

} // namespace

void InstallDebugLogCapture() {
    static bool installed = false;
    if (installed) return;
    installed = true;
    static MD3::DebugStreamBuf coutCapture{std::cout.rdbuf(), MD3::LogLevel::Info};
    static MD3::DebugStreamBuf cerrCapture{std::cerr.rdbuf(), MD3::LogLevel::Error};
    static MD3::DebugStreamBuf clogCapture{std::clog.rdbuf(), MD3::LogLevel::Info};
    std::cout.rdbuf(&coutCapture);
    std::cerr.rdbuf(&cerrCapture);
    std::clog.rdbuf(&clogCapture);
}

void RenderMd3Panel(ParticleSaturn::App::AppController& controller, const char* backendName, std::uint32_t fps,
                    const Md3PanelBackendFeatures& features, const Md3PanelCallbacks& callbacks,
                    const Md3PanelHandTrackingStatus& handStatus) {
    // FPS 历史与面板可见性无关，持续采样，保证打开面板时曲线已就绪。
    static FpsHistoryTracker fpsHistory;
    fpsHistory.AddFrame(ImGui::GetIO().DeltaTime);
    // 诊断记录同样持续汇入日志，面板关闭期间不丢失。
    SyncDiagnosticsToLog();

    auto& state = controller.MutableState();
    if (!state.ui.showDebugWindow) return;

    constexpr float dpi = 1.0f;
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 400.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 200.0f), ImVec2(1200.0f, 1200.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Particle Saturn", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 panelPosition = ImGui::GetWindowPos();
    const ImVec2 panelSize = ImGui::GetWindowSize();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    if (state.ui.blurEnabled && callbacks.drawAcrylicBackground) {
        callbacks.drawAcrylicBackground(drawList, panelPosition, panelSize, 12.0f * dpi);
    } else {
        const auto& colors = MD3::GetContext().colors;
        drawList->AddRectFilled(panelPosition, ImVec2(panelPosition.x + panelSize.x, panelPosition.y + panelSize.y),
                                MD3::ColorToU32(colors.surfaceContainerLow), 12.0f * dpi);
    }
    ImGui::PopStyleColor(4);

    MD3::WindowTitleBarSpace();
    if (MD3::BeginCollapsingHeader("Performance", true)) {
        if (ImGui::BeginTable("PerformanceTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 100.0f * dpi);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            LabeledRow("Backend", "%s", backendName);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("FPS");
            ImGui::TableNextColumn();
            const ImVec4 fpsColor = fps >= 50 ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                                  : fps >= 30 ? ImVec4(1.0f, 0.8f, 0.0f, 1.0f)
                                              : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(fpsColor, "%u", fps);
            LabeledRow("Particles", "%u / %u", state.render.particleCount,
                       ParticleSaturn::App::RenderSettings::MaxParticles);
            LabeledRow("Pixel ratio", "%.2f", state.render.pixelRatio);
            LabeledRow("Resolution", "%u x %u", state.window.width, state.window.height);
            ImGui::EndTable();
        }
        DrawFpsHistoryGraph(fpsHistory, state, callbacks, dpi);
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Visuals", true)) {
        bool darkMode = state.ui.darkMode;
        if (MD3::Toggle("Dark mode", &darkMode)) {
            DispatchAndSave(controller, ParticleSaturn::App::SetDarkMode{darkMode}, callbacks);
            MD3::SetDarkMode(darkMode);
        }
        bool blur = state.ui.blurEnabled;
        if (MD3::Toggle("UI blur", &blur)) DispatchAndSave(controller, ParticleSaturn::App::SetBlurEnabled{blur}, callbacks);
        float blurStrength = state.ui.blurStrength;
        if (MD3::Slider("Blur strength", &blurStrength, 0.0f, 5.0f, "%.1f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetBlurStrength{blurStrength}, callbacks);
        }
        float noise = state.ui.noiseIntensity;
        if (MD3::Slider("Noise", &noise, 0.0f, 0.03f, "%.3f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetNoiseIntensity{noise}, callbacks);
        }
        bool bloom = state.render.bloomEnabled;
        if (MD3::Toggle("Bloom", &bloom)) DispatchAndSave(controller, ParticleSaturn::App::SetBloomEnabled{bloom}, callbacks);
        float bloomStrength = state.render.bloomBlurStrength;
        if (MD3::Slider("Bloom radius", &bloomStrength, 0.0f, 5.0f, "%.1f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetBloomBlurStrength{bloomStrength}, callbacks);
        }
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Window", true)) {
        constexpr const char* materials[] = {"Solid", "Transparent", "System blur", "App Acrylic"};
        int material = static_cast<int>(state.window.material);
        if (MD3::Combo("Window material", &material, materials, IM_ARRAYSIZE(materials))) {
            const auto selected = static_cast<ParticleSaturn::App::WindowMaterial>(material);
            DispatchAndSave(controller, ParticleSaturn::App::SetWindowMaterial{selected}, callbacks);
            if (callbacks.applyWindowMaterial) callbacks.applyWindowMaterial(selected);
        }
        ImGui::TextDisabled("Current: %s", MaterialLabel(state.window.material));
        constexpr const char* vsyncModes[] = {"Off", "On", "Adaptive"};
        int vsync = state.render.vsyncMode == 0 ? 0 : state.render.vsyncMode == 1 ? 1 : 2;
        if (MD3::Combo("VSync", &vsync, vsyncModes, IM_ARRAYSIZE(vsyncModes))) {
            DispatchAndSave(controller, ParticleSaturn::App::SetVSyncMode{vsync == 0 ? 0 : vsync == 1 ? 1 : -1}, callbacks);
        }
        if (MD3::TonalButton(state.window.fullscreen ? "Exit fullscreen" : "Fullscreen")) {
            if (callbacks.toggleFullscreen) callbacks.toggleFullscreen();
        }
        ImGui::SameLine();
        if (MD3::TonalButton("Camera")) {
            if (callbacks.showCameraSelector) callbacks.showCameraSelector();
        }
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Gestures")) {
        RenderHandTrackingStatusCard(handStatus, dpi);
        ImGui::Separator();

        // 原始手势数值（追踪器输出的归一化姿态）。
        if (ImGui::BeginTable("HandDataTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f * dpi);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            LabeledRow("Scale", "%.3f", handStatus.rawScale);
            LabeledRow("Rot X", "%.3f", handStatus.rawRotX);
            LabeledRow("Rot Y", "%.3f", handStatus.rawRotY);
            ImGui::EndTable();
        }

        ImGui::Separator();
        // 平滑后的动画值（FrameCoordinator 输出到场景的状态）。
        ImGui::TextDisabled("Animation values:");
        if (ImGui::BeginTable("AnimTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f * dpi);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            LabeledRow("Scale", "%.3f", state.scene.zoom);
            LabeledRow("Rot X", "%.3f", state.scene.rotationX);
            LabeledRow("Rot Y", "%.3f", state.scene.rotationY);
            ImGui::EndTable();
        }

        ImGui::Separator();
        float sensitivity = state.gesture.sensitivity;
        if (MD3::Slider("Sensitivity", &sensitivity, 0.1f, 3.0f, "%.2f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetGestureSensitivity{sensitivity}, callbacks);
        }
        bool invertX = state.gesture.invertX;
        if (MD3::Toggle("Invert horizontal", &invertX)) DispatchAndSave(controller, ParticleSaturn::App::SetGestureInvertX{invertX}, callbacks);
        ImGui::SameLine(0.0f, 20.0f);
        bool invertY = state.gesture.invertY;
        if (MD3::Toggle("Invert vertical", &invertY)) DispatchAndSave(controller, ParticleSaturn::App::SetGestureInvertY{invertY}, callbacks);
        float lostDelay = static_cast<float>(state.gesture.handLostDelay);
        if (MD3::Slider("Hand lost delay", &lostDelay, 1.0f, 30.0f, "%.0f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetHandLostDelay{static_cast<int>(lostDelay)}, callbacks);
        }
        if (MD3::TonalButton("Reset defaults")) {
            controller.Dispatch(ParticleSaturn::App::SetGestureSensitivity{1.0f});
            controller.Dispatch(ParticleSaturn::App::SetGestureInvertX{false});
            controller.Dispatch(ParticleSaturn::App::SetGestureInvertY{false});
            controller.Dispatch(ParticleSaturn::App::SetHandLostDelay{10});
            if (callbacks.save) callbacks.save();
        }
        ImGui::Separator();
        bool cameraDebug = state.ui.showCameraDebug;
        if (MD3::Toggle("Camera debug", &cameraDebug)) {
            DispatchAndSave(controller, ParticleSaturn::App::SetShowCameraDebug{cameraDebug}, callbacks);
        }
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Advanced")) {
        constexpr const char* apis[] = {"OpenGL 4.1", "Vulkan", "Metal"};
        int api = static_cast<int>(state.render.graphicsApi);
        if (MD3::Combo("Graphics API", &api, apis, IM_ARRAYSIZE(apis))) {
            const auto effect = controller.Dispatch(ParticleSaturn::App::SetGraphicsApi{
                static_cast<ParticleSaturn::App::GraphicsApi>(api)});
            if (callbacks.save) callbacks.save();
            if (effect.restartRequired && callbacks.restartApplication) callbacks.restartApplication();
        }
        ImGui::TextDisabled("Current: %s", ApiLabel(state.render.graphicsApi));
        if (state.render.graphicsApi == ParticleSaturn::App::GraphicsApi::Vulkan) {
            constexpr const char* drivers[] = {"MoltenVK", "KosmicKrisp"};
            int driver = static_cast<int>(state.render.vulkanDriver);
            if (MD3::Combo("Vulkan driver", &driver, drivers, IM_ARRAYSIZE(drivers))) {
                const auto effect = controller.Dispatch(ParticleSaturn::App::SetVulkanDriver{
                    static_cast<ParticleSaturn::App::VulkanDriver>(driver)});
                if (callbacks.save) callbacks.save();
                if (effect.restartRequired && callbacks.restartApplication) callbacks.restartApplication();
            }
        }
        if (features.analyticParticles) {
            bool analytic = state.render.analyticParticles;
            if (MD3::Toggle("Analytic particles", &analytic)) {
                DispatchAndSave(controller, ParticleSaturn::App::SetAnalyticParticles{analytic}, callbacks);
            }
        }
        if (features.objectShaderParticles) {
            bool objectShader = state.render.useObjectShader;
            if (MD3::Toggle("Object shader (Metal 3)", &objectShader)) {
                DispatchAndSave(controller, ParticleSaturn::App::SetUseObjectShader{objectShader}, callbacks);
            }
        }
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("LOD control")) {
        bool lod = state.lod.locked;
        if (MD3::Toggle("Lock dynamic LOD", &lod)) DispatchAndSave(controller, ParticleSaturn::App::SetLodLocked{lod}, callbacks);
        float particleCount = static_cast<float>(state.render.particleCount);
        if (MD3::Slider("Particle count", &particleCount,
                        static_cast<float>(ParticleSaturn::App::RenderSettings::MinParticles),
                        static_cast<float>(ParticleSaturn::App::RenderSettings::MaxParticles), "%.0f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetParticleCount{static_cast<std::uint32_t>(particleCount)}, callbacks);
        }
        float pixelRatio = state.render.pixelRatio;
        if (MD3::Slider("Pixel ratio", &pixelRatio, 0.25f, 1.0f, "%.2f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetPixelRatio{pixelRatio}, callbacks);
        }
        float density = state.render.densityCompensation;
        if (MD3::Slider("Density compensation", &density, 0.0f, 2.0f, "%.2f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetDensityCompensation{density}, callbacks);
        }
        ImGui::TextDisabled("Smoothed frame: %.2f ms", state.lod.smoothedFrameSeconds * 1000.0f);
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Scene")) {
        if (MD3::FilledButton(state.scene.paused ? "Resume" : "Pause")) {
            DispatchAndSave(controller, ParticleSaturn::App::TogglePause{}, callbacks);
        }
        ImGui::TextDisabled("Zoom: %.2f", state.scene.zoom);
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Diagnostics")) {
        ParticleSaturn::Services::Diagnostics::Record record;
        if (!ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Latest(record)) {
            ImGui::TextDisabled("No diagnostics");
        } else {
            const ImVec4 color = record.severity == ParticleSaturn::Services::Diagnostics::Severity::Error
                ? MD3::GetContext().colors.error : MD3::GetContext().colors.primary;
            ImGui::TextColored(color, "%s: %s", record.domain.c_str(), record.code.c_str());
            ImGui::TextWrapped("%s", record.message.c_str());
        }
        MD3::EndCollapsingHeader();
    }

    if (MD3::BeginCollapsingHeader("Log", true)) {
        RenderLogSection(dpi);
        MD3::EndCollapsingHeader();
    }

    MD3::DrawRipples();
    MD3::HandleSmoothScroll(90.0f);
    MD3::WindowScrollbar(40.0f);
    MD3::WindowResize(280.0f, 200.0f);
    bool panelOpen = state.ui.showDebugWindow;
    MD3::WindowTitleBar("Particle Saturn", &panelOpen);
    if (!panelOpen) DispatchAndSave(controller, ParticleSaturn::App::ToggleDebugWindow{}, callbacks);
    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace ParticleSaturn::Platform::MacOS
