// D-015 Phase A：DiligentBackend 的 MD3 调试/设置面板定义搬迁到 Windows 平台外壳目录。
//
// 本文件仍是 DiligentBackend 的成员函数 RenderDebugPanel() 的定义——函数体逐字保留、
// 行为与原先内联在 DiligentBackend.cpp 的版本完全一致。此步只把 UI 代码从 GPU 叶子 TU
// 物理移出、与其它 src/platform/windows 外壳单元并置，缩小 DiligentBackend.cpp 体量。
// 后续（Phase C）再评估解耦为自由函数 + 上下文外观（context facade）。
#include "DiligentBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "DebugLog.h"
#include "Localization.h"
#include "Settings.h"

#include "HandTracker.h"
#include "HandTrackerController.h"
#include "imgui.h"
#include "md3/MD3.h"

namespace ParticleSaturn::Render {

using namespace Diligent;

void DiligentBackend::RenderDebugPanel() {
        // Debug 窗口（默认关闭，F3 切换）- 使用 MD3 无标题栏样式
        if (appState_ != nullptr && appState_->ui.showDebugWindow) {
            ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSizeConstraints(ImVec2(280, 200), ImVec2(1200, 1200));
            constexpr ImGuiWindowFlags kDebugWindowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                                           ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
            ImGui::Begin("Debug", nullptr, kDebugWindowFlags);

            // 自定义标题栏
            constexpr float kTitleBarHeight = 40.0f;
            const auto&     str             = i18n::Get();
            MD3::WindowTitleBar(str.debugPanelTitle, &appState_->ui.showDebugWindow);

            // 绘制窗口背景
            {
                ImVec2      pos   = ImGui::GetWindowPos();
                ImVec2      size  = ImGui::GetWindowSize();
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                ImGuiStyle& style = ImGui::GetStyle();

                auto&  colors       = MD3::GetContext().colors;
                auto&  ctx          = MD3::GetContext();
                float  cornerRadius = style.WindowRounding;
                ImVec2 endPos       = ImVec2(pos.x + size.x, pos.y + size.y);

                // 模糊背景：如果启用且有有效纹理
                const bool    wantBlur = (appState_ != nullptr) ? appState_->ui.enableBlur : false;
                ITextureView* blurSRV =
                    (wantBlur && uiAcrylicSRV_Strong_ != nullptr) ? uiAcrylicSRV_Strong_.RawPtr() : nullptr;
                if (wantBlur) {
                    static bool s_warnedBlurSrvNull  = false;
                    static bool s_warnedNoiseSrvNull = false;
                    if (!s_warnedBlurSrvNull && uiAcrylicSRV_Strong_ == nullptr) {
                        OutputDebugStringA("[DiligentBackend] UI blur enabled but uiAcrylicSRV_Strong_ is null\n");
                        s_warnedBlurSrvNull = true;
                    }
                    if (!s_warnedNoiseSrvNull && uiNoiseSRV_ == nullptr) {
                        OutputDebugStringA("[DiligentBackend] UI blur enabled but uiNoiseSRV_ is null\n");
                        s_warnedNoiseSrvNull = true;
                    }
                }

                if (blurSRV != nullptr && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
                    // UV 计算：D3D12/Vulkan 纹理坐标系（Y 从上到下，无需翻转 Y）
                    ImVec2 uv0 = ImVec2(pos.x / ctx.screenWidth, pos.y / ctx.screenHeight);
                    ImVec2 uv1 = ImVec2(endPos.x / ctx.screenWidth, endPos.y / ctx.screenHeight);

                    // 使用带圆角的图片绘制，避免黑边
                    MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(blurSRV), pos, endPos, uv0, uv1,
                                         IM_COL32(255, 255, 255, 255), cornerRadius);

                    // 噪点层：防 banding + 增加“材质感”
                    if (wantBlur && uiNoiseSRV_ != nullptr) {
                        const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                        const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                        const ImU32 noiseCol  = IM_COL32(255, 255, 255, a);
                        MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(uiNoiseSRV_.RawPtr()), pos, endPos, uv0,
                                             uv1, noiseCol, cornerRadius);
                    }

                    // 高光边框
                    ImU32 highlight =
                        appState_->ui.isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
                    dl->AddRect(pos, endPos, highlight, cornerRadius, 0, 1.0f);
                } else {
                    // 无模糊时的纯色背景
                    ImVec4 bgCol = colors.surfaceContainerLow;
                    bgCol.w      = 0.95f;
                    dl->AddRectFilled(pos, endPos, ImGui::GetColorU32(bgCol), cornerRadius);
                }
            }

            // ========== 性能区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionPerformance, true)) {
                // 两列布局的辅助 lambda
                auto TwoColumnText = [](const char* label, const char* fmt, ...) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", label);
                    ImGui::TableNextColumn();
                    va_list args;
                    va_start(args, fmt);
                    char buf[128];
                    vsnprintf(buf, sizeof(buf), fmt, args);
                    va_end(args);
                    ImGui::Text("%s", buf);
                };

                if (ImGui::BeginTable("PerfTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    // FPS（带颜色）- 使用 MD3 色彩方案
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.fps);
                    ImGui::TableNextColumn();
                    auto&  fpsColors = MD3::GetContext().colors;
                    ImVec4 fpsColor  = (currentFps_ >= 50.0f) ? fpsColors.primary
                                     : (currentFps_ >= 30.0f) ? fpsColors.tertiary
                                                              : fpsColors.error;
                    ImGui::TextColored(fpsColor, "%.1f", currentFps_);

                    const uint32_t uiParticleCount =
                        (appState_ != nullptr) ? appState_->render.activeParticleCount : particleCount_;
                    const float uiPixelRatio = (appState_ != nullptr) ? appState_->render.pixelRatio : 1.0f;
                    TwoColumnText(str.particles, "%u / %u", uiParticleCount, kParticleCountMax);
                    TwoColumnText(str.pixelRatio, "%.2f", uiPixelRatio);
                    TwoColumnText(str.resolution, "%u x %u", surfaceSize_.Width, surfaceSize_.Height);
                    TwoColumnText(str.backend, "%s",
                                  backend_ == Backend::D3D11   ? "D3D11"
                                  : backend_ == Backend::D3D12 ? "D3D12"
                                                               : "Vulkan");

                    ImGui::EndTable();
                }

                // FPS 历史曲线
                ImGui::Dummy(ImVec2(0, 5));

                // 获取历史数据，从最旧到最新
                auto getValue = [&](int logicalIdx) -> float {
                    int actualIdx = (fpsHistoryIndex_ + logicalIdx) % kFpsHistorySize;
                    return fpsHistory_[actualIdx];
                };

                // 使用增量更新的 min/max 缓存（仅在必要时重新遍历）
                float dataMin, dataMax;
                if (fpsHistoryCacheDirty_ || fpsHistoryValidCount_ == 0) {
                    // 需要重新计算
                    dataMin    = 0.0f;
                    dataMax    = 0.0f;
                    bool first = true;
                    for (int i = 0; i < kFpsHistorySize; i++) {
                        float v = getValue(i);
                        if (v > 0.0f) {
                            if (first) {
                                dataMin = dataMax = v;
                                first             = false;
                            } else {
                                if (v < dataMin) {
                                    dataMin = v;
                                }
                                if (v > dataMax) {
                                    dataMax = v;
                                }
                            }
                        }
                    }
                    if (!first) {
                        fpsHistoryCachedMin_ = dataMin;
                        fpsHistoryCachedMax_ = dataMax;
                    }
                    fpsHistoryCacheDirty_ = false;
                } else {
                    // 使用缓存值
                    dataMin = fpsHistoryCachedMin_;
                    dataMax = fpsHistoryCachedMax_;
                }

                // 设置最小显示范围
                const float MIN_DISPLAY_RANGE = 30.0f;
                float       dataRange         = dataMax - dataMin;
                if (dataRange < MIN_DISPLAY_RANGE) {
                    float center = (dataMax + dataMin) * 0.5f;
                    dataMin      = center - MIN_DISPLAY_RANGE * 0.5f;
                    dataMax      = center + MIN_DISPLAY_RANGE * 0.5f;
                }

                // 添加边距
                float margin    = (dataMax - dataMin) * 0.1f;
                float targetMin = dataMin - margin;
                float targetMax = dataMax + margin;
                if (targetMin < 0.0f) {
                    targetMin = 0.0f;
                }

                // Y 轴范围动画 - 平滑过渡
                const float kAnimSpeed = 8.0f;
                float       animDt     = ImGui::GetIO().DeltaTime;
                if (fpsGraphFirstFrame_) {
                    fpsGraphAnimMinVal_ = targetMin;
                    fpsGraphAnimMaxVal_ = targetMax;
                    fpsGraphFirstFrame_ = false;
                } else {
                    float decay         = expf(-kAnimSpeed * animDt);
                    fpsGraphAnimMinVal_ = fpsGraphAnimMinVal_ * decay + targetMin * (1.0f - decay);
                    fpsGraphAnimMaxVal_ = fpsGraphAnimMaxVal_ * decay + targetMax * (1.0f - decay);
                }

                float minVal   = fpsGraphAnimMinVal_;
                float maxVal   = fpsGraphAnimMaxVal_;
                float valRange = maxVal - minVal;
                if (valRange < 1.0f) {
                    valRange = 1.0f;
                }

                // 绘图区域（调整右边距与左侧对齐 - 使用折叠区域的 contentPadding）
                float       contentIndent = 16.0f * appState_->ui.dpiScale;
                ImVec2      plotSize(ImGui::GetContentRegionAvail().x - contentIndent, 50);
                ImVec2      plotPos = ImGui::GetCursorScreenPos();
                ImVec2      plotEnd(plotPos.x + plotSize.x, plotPos.y + plotSize.y);
                ImDrawList* drawList     = ImGui::GetWindowDrawList();
                float       cornerRadius = 6.0f * appState_->ui.dpiScale;

                // 背景
                auto& ctx       = MD3::GetContext();
                auto& colors    = ctx.colors;
                ImU32 lineColor = ImGui::GetColorU32(colors.primary);
                ImU32 axisColor = IM_COL32(180, 180, 180, 140); // 半透明灰色用于 Y 轴标签

                // 如果启用模糊，绘制 Acrylic 效果背景
                if (ctx.blurEnabled && ctx.blurTextureID2 != nullptr && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
                    // UV 计算：D3D12/Vulkan 纹理坐标系（Y 从上到下）
                    ImVec2 uv0(plotPos.x / ctx.screenWidth, plotPos.y / ctx.screenHeight);
                    ImVec2 uv1(plotEnd.x / ctx.screenWidth, plotEnd.y / ctx.screenHeight);

                    // 弱模糊背景
                    MD3::AddImageRounded(drawList, reinterpret_cast<ImTextureID>(ctx.blurTextureID2), plotPos, plotEnd,
                                         uv0, uv1, IM_COL32(255, 255, 255, 255), cornerRadius);

                    // 噪点层：防 banding + 增加“材质感”
                    if (ctx.noiseTextureID != nullptr) {
                        const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                        const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                        const ImU32 noiseCol  = IM_COL32(255, 255, 255, a);
                        MD3::AddImageRounded(drawList, reinterpret_cast<ImTextureID>(ctx.noiseTextureID), plotPos,
                                             plotEnd, uv0, uv1, noiseCol, cornerRadius);
                    }

                    // 细边框
                    ImU32 borderColor = ctx.isDarkMode ? IM_COL32(255, 255, 255, 30) : IM_COL32(0, 0, 0, 20);
                    drawList->AddRect(plotPos, plotEnd, borderColor, cornerRadius, 0, 1.0f);
                } else {
                    // 无模糊时的纯色背景
                    ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_FrameBg);
                    drawList->AddRectFilled(plotPos, plotEnd, bgColor, cornerRadius);
                }

                // 裁剪区域
                drawList->PushClipRect(plotPos, plotEnd, true);

                // 转换坐标（含滚动动画）
                // 计算滚动进度：使用 EaseOutCubic 缓动使动画开始快结束慢
                auto easeOutCubic = [](float t) -> float {
                    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
                    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
                };
                float scrollProgress = easeOutCubic(fpsGraphScrollAnimTime_ / kFpsHistorySampleInterval);

                auto toScreen = [&](int logicalIdx, float val) -> ImVec2 {
                    // 修复：最新数据点始终固定在右边界，滚动只影响较旧的点向左偏移
                    // normalizedX: 将逻辑索引 [0, N-1] 映射到 [0, 1]
                    float normalizedX = (float)logicalIdx / (float)(kFpsHistorySize - 1);
                    // scrollOffset: 新数据到来时从 0 渐变到 1/(N-1)，所有点同步左移
                    float scrollOffset = scrollProgress / (float)(kFpsHistorySize - 1);
                    float x            = plotPos.x + (normalizedX - scrollOffset) * plotSize.x;

                    float clampedVal = val < minVal ? minVal : (val > maxVal ? maxVal : val);
                    float y          = plotPos.y + plotSize.y - ((clampedVal - minVal) / valRange) * plotSize.y;
                    return ImVec2(x, y);
                };

                // 绘制曲线（Catmull-Rom 样条插值）
                ImVector<ImVec2> dataPoints;
                dataPoints.reserve(kFpsHistorySize);
                for (int i = 0; i < kFpsHistorySize; i++) {
                    float val = getValue(i);
                    if (val > 0.0f) {
                        dataPoints.push_back(toScreen(i, val));
                    }
                }

                if (dataPoints.Size >= 2) {
                    // Catmull-Rom 样条插值生成平滑曲线
                    ImVector<ImVec2> smoothPoints;
                    const int        kSegmentsPerSpan = 8; // 每两个数据点之间插入的段数
                    smoothPoints.reserve(dataPoints.Size * kSegmentsPerSpan);

                    for (int i = 0; i < dataPoints.Size - 1; i++) {
                        // 获取控制点 p0, p1, p2, p3
                        ImVec2 p0 = (i > 0) ? dataPoints[i - 1] : dataPoints[i];
                        ImVec2 p1 = dataPoints[i];
                        ImVec2 p2 = dataPoints[i + 1];
                        ImVec2 p3 = (i + 2 < dataPoints.Size) ? dataPoints[i + 2] : dataPoints[i + 1];

                        // Catmull-Rom 插值
                        for (int s = 0; s < kSegmentsPerSpan; s++) {
                            float t  = (float)s / (float)kSegmentsPerSpan;
                            float t2 = t * t;
                            float t3 = t2 * t;

                            // Catmull-Rom 基函数
                            float b0 = -0.5f * t3 + t2 - 0.5f * t;
                            float b1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
                            float b2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
                            float b3 = 0.5f * t3 - 0.5f * t2;

                            ImVec2 pt;
                            pt.x = b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x;
                            pt.y = b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y;
                            smoothPoints.push_back(pt);
                        }
                    }
                    // 添加最后一个点
                    smoothPoints.push_back(dataPoints[dataPoints.Size - 1]);

                    drawList->AddPolyline(smoothPoints.Data, smoothPoints.Size, lineColor, ImDrawFlags_None, 2.0f);
                }

                drawList->PopClipRect();

                // Y 轴刻度（较小字体，半透明）
                char maxLabel[16], minLabel[16];
                snprintf(maxLabel, sizeof(maxLabel), "%.0f", maxVal);
                snprintf(minLabel, sizeof(minLabel), "%.0f", minVal);
                float smallFontSize = ImGui::GetFontSize() * 0.85f;
                drawList->AddText(ImGui::GetFont(), smallFontSize, ImVec2(plotPos.x + 4, plotPos.y + 2), axisColor,
                                  maxLabel);
                drawList->AddText(ImGui::GetFont(), smallFontSize,
                                  ImVec2(plotPos.x + 4, plotPos.y + plotSize.y - smallFontSize - 2), axisColor,
                                  minLabel);

                ImGui::Dummy(plotSize);

                // Tooltip
                if (ImGui::IsItemHovered()) {
                    ImVec2 mousePos = ImGui::GetIO().MousePos;
                    float  relX     = (mousePos.x - plotPos.x) / plotSize.x;
                    int    idx      = (int)(relX * (float)(kFpsHistorySize - 1) + 0.5f);
                    if (idx >= 0 && idx < kFpsHistorySize) {
                        float fpsVal = getValue(idx);
                        ImGui::BeginTooltip();
                        ImGui::Text("%.0f FPS", fpsVal);
                        ImGui::EndTooltip();
                    }
                }
                MD3::EndCollapsingHeader();
            }

            // ========== 手部追踪区域（HandTracker 集成）==========
            if (MD3::BeginCollapsingHeader(str.sectionHandTracking, true)) {
                HandTracking::Status st = HandTracking::Status::Unavailable;
                if (handTracker_ != nullptr) {
                    st = handTracker_->GetStatus();
                }

                const char* statusText = str.trackerUnavailable;
                ImVec4      statusCol  = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                switch (st) {
                case HandTracking::Status::NotStarted:
                    statusText = str.trackerNotStarted;
                    statusCol  = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
                    break;
                case HandTracking::Status::Starting:
                    statusText = str.trackerInitializing;
                    statusCol  = ImVec4(1.0f, 0.8f, 0.0f, 1.0f);
                    break;
                case HandTracking::Status::Ready:
                    statusText = str.trackerReady;
                    statusCol  = ImVec4(0.3f, 1.0f, 0.3f, 1.0f);
                    break;
                case HandTracking::Status::Failed:
                    statusText = str.trackerFailed;
                    statusCol  = ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
                    break;
                case HandTracking::Status::Unavailable:
                default:
                    statusText = str.trackerUnavailable;
                    statusCol  = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);
                    break;
                }

                // 控制：重启并选择摄像头
                if (MD3::TonalButton(str.cameraSelectorButton)) {
                    if (handTracker_ != nullptr) {
                        handTracker_->RestartWithCameraSelector(true);
                    }
                }
                ImGui::TextDisabled("%s: #%d", str.selectedCamera,
                                    handTracker_ ? handTracker_->GetSelectedCamera() : -1);

                if (ImGui::BeginTable("TrackerTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.labelStatus);
                    ImGui::TableNextColumn();
                    ImGui::TextColored(statusCol, "%s", statusText);

                    if (st == HandTracking::Status::Failed) {
                        const int errCode = handTracker_ ? handTracker_->GetLastErrorCode() : HANDTRACKER_ERROR_UNKNOWN;
                        const auto errMsg = handTracker_ ? handTracker_->GetLastErrorMessageUtf8() : std::string{};

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.labelErrorCode);
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", errCode);

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", str.labelErrorMessage);
                        ImGui::TableNextColumn();
                        ImGui::TextWrapped("%s", errMsg.empty() ? "-" : errMsg.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::Separator();

                // 用户可调参数：让它“真的生效”
                if (appState_ != nullptr) {
                    ImGui::Text("%s:", str.sensitivity);
                    MD3::Slider("##HandSensitivity", &appState_->handParams.sensitivity, 0.1f, 3.0f, "%.2f");
                    MD3::Toggle(str.invertX, &appState_->handParams.invertX);
                    MD3::Toggle(str.invertY, &appState_->handParams.invertY);

                    ImGui::Text("%s (%s):", str.handLostDelay, str.frames);
                    float delayF = static_cast<float>(appState_->handParams.handLostDelay);
                    if (MD3::Slider("##HandLostDelay", &delayF, 1.0f, 30.0f, "%.0f")) {
                        appState_->handParams.handLostDelay = static_cast<int>(delayF);
                    }
                }

                ImGui::Separator();

                // 追踪器调试开关
                bool debugEnabled = false;
                if (handTracker_ != nullptr && handTracker_->GetDebugMode(&debugEnabled)) {
                    if (MD3::Toggle(str.showCameraDebug, &debugEnabled)) {
                        handTracker_->SetDebugMode(debugEnabled);
                        if (appState_ != nullptr) {
                            appState_->ui.showCameraDebug = debugEnabled;
                        }
                    }
                } else {
                    ImGui::TextDisabled("%s: (%s)", str.showCameraDebug, str.notAvailable);
                }

                // SIMD mode
                ImGui::TextDisabled("%s:", str.simdMode);
                int simdMode = 0;
                if (handTracker_ != nullptr && handTracker_->GetSIMDMode(&simdMode)) {
                    const char* simdModes[] = {str.simdAuto, str.simdAVX2, str.simdSSE, str.simdScalar};
                    if (MD3::Combo(str.simdMode, &simdMode, simdModes, 4)) {
                        handTracker_->SetSIMDMode(simdMode);
                    }
                    const std::string impl = handTracker_->GetSIMDImplementation();
                    ImGui::Text("%s: %s", str.simdCurrent, impl.empty() ? str.statusUnknown : impl.c_str());
                } else {
                    ImGui::TextDisabled("%s: (%s)", str.simdMode, str.notAvailable);
                }

                ImGui::Separator();

                // 实时数值：raw vs smoothed（便于调试）
                HandTracking::Sample raw{};
                if (handTracker_ != nullptr && handTracker_->GetStatus() == HandTracking::Status::Ready) {
                    raw = handTracker_->GetLatestSample();
                }

                ImGui::TextDisabled("%s:", str.rawHandTrackerValues);
                if (ImGui::BeginTable("RawTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.handDetected);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", raw.hasHand ? str.yes : str.no);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.scale);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.scale);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotX);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.rotX);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotY);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", raw.rotY);

                    ImGui::EndTable();
                }

                ImGui::Separator();

                ImGui::TextDisabled("%s:", str.smoothedAnimationValues);
                if (ImGui::BeginTable("AnimTable", 2, ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.scale);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animScale_);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotX);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animRotX_);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", str.animationRotY);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", animRotY_);

                    ImGui::EndTable();
                }

                MD3::EndCollapsingHeader();
            }

            // ========== 视觉效果区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionVisuals)) {
                // 暗色模式切换 - 使用 MD3 Toggle
                if (MD3::Toggle(str.darkMode, &appState_->ui.isDarkMode)) {
                    // 应用 MD3 主题样式
                    MD3::ApplyImGuiStyle();
                }

                // Bloom 辉光效果开关
                MD3::Toggle(str.bloom, &bloomEnabled_);

                // 启用时显示强度滑块
                if (bloomEnabled_) {
                    ImGui::TextUnformatted(str.bloomStrength);
                    MD3::Slider("##BloomIntensity", &bloomStrength_, 0.1f, 1.5f, "%.2f");
                }

                ImGui::Spacing();

                // 玻璃模糊效果开关（窗口背景）
                MD3::Toggle(str.glassBlur, &appState_->ui.enableBlur);

                // 启用时显示强度滑块
                if (appState_->ui.enableBlur) {
                    ImGui::TextUnformatted(str.blurStrength);
                    MD3::Slider("##BlurStr", &appState_->ui.blurStrength, 0.0f, 5.0f, "%.1f");
                    ImGui::TextUnformatted(str.noise);
                    MD3::Slider("##Noise", &appState_->ui.noiseIntensity, 0.0f, 0.03f, "%.3f");
                }

                MD3::EndCollapsingHeader();
            }

            // ========== 窗口区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionWindow)) {
                // 图形后端切换
                ImGui::Text("%s:", str.switchBackend);
                int         backendIndex   = static_cast<int>(backend_);
                const char* backendNames[] = {"D3D11", "D3D12", "Vulkan"};
                if (MD3::Combo("##BackendSwitch", &backendIndex, backendNames, 3)) {
                    if (backendIndex != static_cast<int>(backend_)) {
                        // 用户选择了不同的后端，触发重启
                        auto newBackend = static_cast<Backend>(backendIndex);
                        if (Settings::RestartWithBackend(newBackend, *appState_)) {
                            PostQuitMessage(0);
                        }
                    }
                }
                ImGui::TextDisabled("%s", str.switchBackendConfirm);

                // Mesh Shader 开关（仅在 D3D12 且硬件支持时显示）
                if (meshShaderSupported_) {
                    ImGui::Dummy(ImVec2(0, 5));
                    if (MD3::Toggle(str.meshShader, &useMeshShaders_)) {
                        if (useMeshShaders_) {
                            DebugLog::Instance().Add(LogLevel::Info, "[GPU] Mesh Shader enabled by user");
                        } else {
                            DebugLog::Instance().Add(LogLevel::Info,
                                                     "[GPU] Mesh Shader disabled by user, using Vertex Pulling");
                        }
                    }
                }

                ImGui::Dummy(ImVec2(0, 5));

                // VSync 模式选择 - 使用 MD3 Combo
                ImGui::Text("%s:", str.vsync);
                int vsyncIndex = 1;
                if (appState_->render.vsyncMode == 0) {
                    vsyncIndex = 0;
                } else if (appState_->render.vsyncMode == 1) {
                    vsyncIndex = 1;
                } else {
                    vsyncIndex = 2; // -1 (Adaptive)
                }

                if (appState_->render.adaptiveVSyncSupported) {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn, str.vsyncAdaptive};
                    if (MD3::Combo("##VSync", &vsyncIndex, vsyncModes, 3)) {
                        appState_->render.vsyncMode = (vsyncIndex == 0) ? 0 : (vsyncIndex == 1) ? 1 : -1;
                    }
                } else {
                    const char* vsyncModes[] = {str.vsyncOff, str.vsyncOn};
                    if (vsyncIndex > 1) {
                        vsyncIndex = 1; // 不支持 Adaptive 时回退到 On
                    }
                    if (MD3::Combo("##VSync", &vsyncIndex, vsyncModes, 2)) {
                        appState_->render.vsyncMode = vsyncIndex; // 0/1
                    }
                }

                ImGui::Dummy(ImVec2(0, 5));

                // Backdrop/透明合成开关（简化版：开=使用 Mica，关=Solid）
                // 注意：需要 Win10 1809+ 且窗口支持 DirectComposition；为保证运行期可反复切换不失效，DComp
                // 一旦启用将保持启用。 仅 D3D12 和 D3D11 后端显示此选项。
                if (appState_->backdrop.transparentSupported &&
                    (backend_ == Backend::D3D12 || backend_ == Backend::D3D11)) {
                    bool transparent = appState_->backdrop.useTransparent;
                    if (MD3::Toggle(str.transparent, &transparent)) {
                        // 透明时使用 Mica (mode=3)，不透明时使用 Solid (mode=0)
                        const int newMode = transparent ? 3 : 0;
                        if (SetBackdropMode(newMode)) {
                            // 更新 backdropIndex 以匹配新模式
                            for (int i = 0; i < static_cast<int>(appState_->backdrop.availableBackdrops.size()); ++i) {
                                if (appState_->backdrop.availableBackdrops[i] == newMode) {
                                    appState_->backdrop.backdropIndex = i;
                                    break;
                                }
                            }
                        }
                    }
                }
                // Vulkan 后端或系统不支持时：不显示透明开关

                ImGui::Dummy(ImVec2(0, 5));

                // 显示状态
                ImGui::Text("%s: %s", str.fullscreen, appState_->window.isFullscreen ? str.yes : str.no);
                MD3::EndCollapsingHeader();
            }

            // ========== 高级区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionAdvanced)) {
                // 显示一些调试信息
                ImGui::TextDisabled("%s:", str.debugInfo);
                ImGui::Text("%s: %u", str.starCount, starCount_);
                ImGui::Text("%s: %u x %u", str.offscreen, surfaceSize_.Width, surfaceSize_.Height);

                MD3::EndCollapsingHeader();
            }

            // ========== LOD 控制区域 ==========
            if (MD3::BeginCollapsingHeader(str.sectionLodControl)) {
                // 锁定 LOD 开关
                MD3::Toggle(str.lodLock, &appState_->lod.locked);

                ImGui::Dummy(ImVec2(0, 5));

                // 粒子数量滑块
                ImGui::Text("%s:", str.particleCount);
                float particleCount = static_cast<float>(appState_->render.activeParticleCount);
                if (MD3::Slider("##ParticleCount", &particleCount, static_cast<float>(kParticleCountMin),
                                static_cast<float>(kParticleCountMax), "%.0f")) {
                    appState_->render.activeParticleCount = static_cast<uint32_t>(particleCount);
                }

                ImGui::Dummy(ImVec2(0, 5));

                // 像素比例滑块
                ImGui::Text("%s:", str.pixelRatio);
                MD3::Slider("##PixelRatio", &appState_->render.pixelRatio, 0.5f, 1.0f, "%.2f");

                ImGui::Dummy(ImVec2(0, 5));

                // 密度补偿
                ImGui::Text("%s:", str.densityCompensation);
                MD3::Slider("##DensityComp", &appState_->render.densityComp, 0.0f, 2.0f, "%.2f");

                MD3::EndCollapsingHeader();
            }

            // ========== 日志区域 ==========
            // 复刻 OpenGL 版：级别过滤 + 搜索 + 暂停按钮（带图标）+ 清空/复制 + 日志列表
            if (MD3::BeginCollapsingHeader(str.sectionLog, true)) {
                static char logSearchBuffer[128] = "";
                static int  logLevelFilter       = 0; // 0=全部, 1=Info, 2=Warn, 3=Error

                float dpi           = appState_->ui.dpiScale;
                float controlHeight = 40.0f * dpi;   // 与 MD3::Combo 控件高度一致
                float buttonSize    = controlHeight; // 暂停按钮尺寸（正方形）

                // 第一行：级别过滤、搜索和暂停按钮
                ImGui::SetNextItemWidth(80 * dpi);
                const char* levelLabels[] = {str.logLevelAll, str.logLevelInfo, str.logLevelWarn, str.logLevelError};
                MD3::Combo("##LogLevel", &logLevelFilter, levelLabels, 4);

                ImGui::SameLine();
                // 搜索栏宽度 = 可用宽度 - 暂停按钮 - 间距 - 右侧留白
                float itemSpacingX   = ImGui::GetStyle().ItemSpacing.x;
                float rightMargin    = 12.0f * dpi; // 让暂停按钮不要贴右边界
                float minSearchWidth = 120.0f * dpi;
                float contentAvailX  = ImGui::GetContentRegionAvail().x;

                float searchWidth = contentAvailX - buttonSize - itemSpacingX - rightMargin;
                if (searchWidth < minSearchWidth) {
                    searchWidth = contentAvailX - buttonSize - itemSpacingX;
                }
                if (searchWidth < 1.0f) {
                    searchWidth = 1.0f;
                }

                ImGui::SetNextItemWidth(searchWidth);

                // InputText 走 ImGui 自带绘制：通过 FramePadding 精确对齐 MD3 的 40dp 高度，
                // 并用圆角裁剪避免文字“顶出”圆角区域
                float padY = (controlHeight - ImGui::GetFontSize()) * 0.5f;
                if (padY < 0.0f) {
                    padY = 0.0f;
                }

                float inputRounding = controlHeight * 0.5f;
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16.0f * dpi, padY));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, inputRounding);

                ImVec2 inputPos  = ImGui::GetCursorScreenPos();
                ImVec2 inputSize = ImVec2(searchWidth, controlHeight);
                MD3::PushRoundedClipRect(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y),
                                         inputRounding);
                ImGui::InputTextWithHint("##LogSearch", str.logSearch, logSearchBuffer, sizeof(logSearchBuffer));
                MD3::PopRoundedClipRect();

                ImGui::PopStyleVar(2);

                // 右键粘贴菜单
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
                if (ImGui::BeginPopupContextItem("##LogSearchContext")) {
                    const char* clipText = ImGui::GetClipboardText();
                    bool        canPaste = (clipText && clipText[0] != '\0');
                    if (MD3::MenuItem(str.paste, canPaste, 36.0f * dpi)) {
                        // 追加粘贴内容到搜索栏
                        size_t currentLen = std::strlen(logSearchBuffer);
                        size_t clipLen    = std::strlen(clipText);
                        size_t maxAppend  = sizeof(logSearchBuffer) - 1 - currentLen;
                        if (clipLen > maxAppend) {
                            clipLen = maxAppend;
                        }
                        if (clipLen > 0) {
                            std::memcpy(logSearchBuffer + currentLen, clipText, clipLen);
                            logSearchBuffer[currentLen + clipLen] = '\0';
                        }
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopStyleVar(2);

                // 暂停/继续按钮（MD3 风格按钮 + 图标）
                ImGui::SameLine();
                bool isPaused = DebugLog::Instance().IsPaused();

                ImGui::PushID("LogPauseBtn");
                ImGui::InvisibleButton("##btn", ImVec2(buttonSize, buttonSize));

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2      btnMin   = ImGui::GetItemRectMin();
                ImVec2      btnMax   = ImGui::GetItemRectMax();
                bool        hovered  = ImGui::IsItemHovered();
                bool        held     = ImGui::IsItemActive();
                bool        clicked  = ImGui::IsItemClicked(0);

                const MD3::MD3ColorScheme colors =
                    MD3::IsDarkMode() ? MD3::GetDarkColorScheme() : MD3::GetLightColorScheme();
                float rounding = std::min(12.0f * dpi, buttonSize * 0.5f);

                if (clicked) {
                    MD3::TriggerRippleForCurrentItem(ImGui::GetItemID(), rounding);
                    DebugLog::Instance().SetPaused(!isPaused);
                }

                if (hovered) {
                    ImGui::SetTooltip("%s", isPaused ? str.logResume : str.logPause);
                }

                // 轻微阴影（更接近 MD3 elevation）
                {
                    ImVec4 shadow = colors.shadow;
                    shadow.w      = 0.18f;
                    ImVec2 sMin(btnMin.x, btnMin.y + 1.0f * dpi);
                    ImVec2 sMax(btnMax.x, btnMax.y + 1.0f * dpi);
                    drawList->AddRectFilled(sMin, sMax, MD3::ColorToU32(shadow), rounding);
                }

                // 底色 + 状态层
                ImVec4 bgColor = colors.surfaceContainerHigh;
                if (hovered || held) {
                    float alpha = held ? colors.stateLayerPressed : colors.stateLayerHover;
                    bgColor     = MD3::ApplyStateLayer(bgColor, colors.onSurface, alpha);
                }
                drawList->AddRectFilled(btnMin, btnMax, MD3::ColorToU32(bgColor), rounding);

                // 图标：使用离线烘焙的 alpha 掩码（由原 SVG 转换得到），运行时只上传一次纹理
                float drawIconPx = 24.0f * dpi;
                if (auto* iconSRV = GetOrCreateLogControlIconSRV(isPaused); iconSRV != nullptr) {
                    float  cx = (btnMin.x + btnMax.x) * 0.5f;
                    float  cy = (btnMin.y + btnMax.y) * 0.5f;
                    ImVec2 iconMin(cx - drawIconPx * 0.5f, cy - drawIconPx * 0.5f);
                    ImVec2 iconMax(cx + drawIconPx * 0.5f, cy + drawIconPx * 0.5f);
                    drawList->AddImage(reinterpret_cast<ImTextureID>(iconSRV), iconMin, iconMax, ImVec2(0, 0),
                                       ImVec2(1, 1), IM_COL32_WHITE);
                }

                ImGui::PopID();

                // 第二行：清空和复制按钮
                if (MD3::TonalButton(str.clearLog)) {
                    DebugLog::Instance().Clear();
                }
                ImGui::SameLine();
                if (MD3::TonalButton(str.copyAllLog)) {
                    std::string filteredText = DebugLog::Instance().GetFilteredText(logSearchBuffer, logLevelFilter);
                    ImGui::SetClipboardText(filteredText.c_str());
                }

                // 日志列表（带过滤和搜索）
                DebugLog::Instance().Draw(logSearchBuffer, logLevelFilter);
                MD3::EndCollapsingHeader();
            }

            // 处理平滑滚动（必须在 WindowScrollbar 之前调用）
            MD3::HandleSmoothScroll(90.0f);

            // 自定义滚动条和缩放手柄
            MD3::WindowScrollbar(kTitleBarHeight);
            MD3::WindowResize(280.0f, 200.0f);

            ImGui::End();
        }
}

} // namespace ParticleSaturn::Render
