// D-015 Windows 重启 Phase B（全对称）：MD3 调试/设置面板内容已上移到 Win32 外壳
// （Main.cpp，接缝之上），与 macOS 完全对称——外壳按后端能力组装共享面板契约
// （features/callbacks/handStatus）并调用 ParticleSaturn::UI::RenderMd3Panel。
//
// 本文件只保留后端对面板的两处“接缝以下”贡献：
//   1) BuildPanelHooks()：本帧 acrylic 纹理钩子（强模糊面板背景 + FPS 曲线弱模糊）。
//      这些 SRV 只在后端帧内有效，故经 FrameContext::drawPanel 的 BackendPanelHooks 交付。
//   2) SetMeshShaderEnabled()：Mesh Shader 开关（状态仍由后端持有，决定 PSO 选择），
//      供外壳的面板回调驱动；此处顺带写入 MD3 调试日志。
#include "DiligentBackend.h"

#include <algorithm>

#include "imgui.h"
#include "md3/MD3.h"
#include "md3/MD3Log.h"

namespace ParticleSaturn::Render {

using namespace Diligent;

App::BackendPanelHooks DiligentBackend::BuildPanelHooks() {
    App::BackendPanelHooks hooks;

    // 面板窗口背景（强模糊 Acrylic + 噪点 + 高光边框）。仅在 ui.blurEnabled 时被共享面板调用。
    // D3D12/Vulkan 纹理坐标系 Y 从上到下，无需翻转 Y。
    hooks.drawAcrylicBackground = [this](ImDrawList* dl, const ImVec2& pos, const ImVec2& size, float rounding) {
        const ImVec2  endPos(pos.x + size.x, pos.y + size.y);
        auto&         ctx     = MD3::GetContext();
        ITextureView* blurSRV = (uiAcrylicSRV_Strong_ != nullptr) ? uiAcrylicSRV_Strong_.RawPtr() : nullptr;
        if (blurSRV != nullptr && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
            const ImVec2 uv0(pos.x / ctx.screenWidth, pos.y / ctx.screenHeight);
            const ImVec2 uv1(endPos.x / ctx.screenWidth, endPos.y / ctx.screenHeight);
            MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(blurSRV), pos, endPos, uv0, uv1,
                                 IM_COL32(255, 255, 255, 255), rounding);
            if (uiNoiseSRV_ != nullptr) {
                const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                MD3::AddImageRounded(dl, reinterpret_cast<ImTextureID>(uiNoiseSRV_.RawPtr()), pos, endPos, uv0, uv1,
                                     IM_COL32(255, 255, 255, a), rounding);
            }
            const ImU32 highlight =
                (appState_ != nullptr && appState_->ui.darkMode) ? IM_COL32(255, 255, 255, 40)
                                                                 : IM_COL32(255, 255, 255, 120);
            dl->AddRect(pos, endPos, highlight, rounding, 0, 1.0f);
        } else {
            // 模糊已开但强模糊纹理尚未就绪：退回近似不透明纯色，避免窗口透空。
            ImVec4 bgCol = ctx.colors.surfaceContainerLow;
            bgCol.w      = 0.95f;
            dl->AddRectFilled(pos, endPos, ImGui::GetColorU32(bgCol), rounding);
        }
    };

    // FPS 曲线的弱模糊背景（blurTextureID2 + 噪点）。边框由共享面板统一补绘，此处不画边框。
    hooks.drawGraphAcrylic = [this](ImDrawList* dl, const ImVec2& pos, const ImVec2& size, float rounding) {
        const ImVec2 endPos(pos.x + size.x, pos.y + size.y);
        auto&        ctx = MD3::GetContext();
        if (ctx.blurTextureID2 != 0 && ctx.screenWidth > 0 && ctx.screenHeight > 0) {
            const ImVec2 uv0(pos.x / ctx.screenWidth, pos.y / ctx.screenHeight);
            const ImVec2 uv1(endPos.x / ctx.screenWidth, endPos.y / ctx.screenHeight);
            MD3::AddImageRounded(dl, ctx.blurTextureID2, pos, endPos, uv0, uv1, IM_COL32(255, 255, 255, 255), rounding);
            if (ctx.noiseTextureID != 0) {
                const float intensity = std::clamp(ctx.noiseIntensity, 0.0f, 0.1f);
                const int   a         = std::clamp(static_cast<int>(intensity * 255.0f + 0.5f), 0, 64);
                MD3::AddImageRounded(dl, ctx.noiseTextureID, pos, endPos, uv0, uv1, IM_COL32(255, 255, 255, a),
                                     rounding);
            }
        } else {
            dl->AddRectFilled(pos, endPos, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
        }
    };

    return hooks;
}

void DiligentBackend::SetMeshShaderEnabled(bool enabled) {
    useMeshShaders_ = enabled;
    MD3::DebugLog::Instance().Add(MD3::LogLevel::Info,
                                  enabled ? "[GPU] Mesh Shader enabled by user"
                                          : "[GPU] Mesh Shader disabled by user, using Vertex Pulling");
}

} // namespace ParticleSaturn::Render
