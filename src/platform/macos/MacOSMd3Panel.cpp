#include "MacOSMd3Panel.h"

#include <algorithm>

#include "imgui.h"

#include "MD3.h"
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

} // namespace

void RenderMd3Panel(ParticleSaturn::App::AppController& controller, const char* backendName, std::uint32_t fps,
                    bool supportsAnalyticParticles, const Md3PanelCallbacks& callbacks) {
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
        callbacks.drawAcrylicBackground(drawList, panelPosition, panelSize);
    } else {
        const auto& colors = MD3::GetContext().colors;
        drawList->AddRectFilled(panelPosition, ImVec2(panelPosition.x + panelSize.x, panelPosition.y + panelSize.y),
                                MD3::ColorToU32(colors.surfaceContainerLow), 12.0f * dpi);
    }
    ImGui::PopStyleColor(4);

    MD3::WindowTitleBarSpace();
    if (MD3::BeginCollapsingHeader("Performance", true)) {
        if (ImGui::BeginTable("PerformanceTable", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Metric", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Backend");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(backendName);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("FPS");
            ImGui::TableNextColumn(); ImGui::TextColored(fps >= 50 ? MD3::GetContext().colors.primary : MD3::GetContext().colors.error, "%u", fps);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Particles");
            ImGui::TableNextColumn(); ImGui::Text("%u", state.render.particleCount);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("Resolution");
            ImGui::TableNextColumn(); ImGui::Text("%u x %u", state.window.width, state.window.height);
            ImGui::EndTable();
        }
        const ImVec2 graphPos = ImGui::GetCursorScreenPos();
        const ImVec2 graphSize(ImGui::GetContentRegionAvail().x, 48.0f);
        drawList->AddRectFilled(graphPos, ImVec2(graphPos.x + graphSize.x, graphPos.y + graphSize.y),
                                MD3::ColorToU32(MD3::GetContext().colors.surfaceContainerHigh), 6.0f);
        const float y = graphPos.y + graphSize.y * (1.0f - std::clamp(fps / 120.0f, 0.0f, 1.0f));
        drawList->AddLine(ImVec2(graphPos.x + 8.0f, y), ImVec2(graphPos.x + graphSize.x - 8.0f, y),
                          MD3::ColorToU32(MD3::GetContext().colors.primary), 2.0f);
        ImGui::Dummy(graphSize);
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
        ImGui::TextDisabled("Camera gesture control");
        float sensitivity = state.gesture.sensitivity;
        if (MD3::Slider("Sensitivity", &sensitivity, 0.1f, 5.0f, "%.2f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetGestureSensitivity{sensitivity}, callbacks);
        }
        bool invertX = state.gesture.invertX;
        if (MD3::Toggle("Invert horizontal", &invertX)) DispatchAndSave(controller, ParticleSaturn::App::SetGestureInvertX{invertX}, callbacks);
        bool invertY = state.gesture.invertY;
        if (MD3::Toggle("Invert vertical", &invertY)) DispatchAndSave(controller, ParticleSaturn::App::SetGestureInvertY{invertY}, callbacks);
        float lostDelay = static_cast<float>(state.gesture.handLostDelay);
        if (MD3::Slider("Hand lost delay", &lostDelay, 1.0f, 30.0f, "%.0f")) {
            DispatchAndSave(controller, ParticleSaturn::App::SetHandLostDelay{static_cast<int>(lostDelay)}, callbacks);
        }
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
        if (supportsAnalyticParticles) {
            bool analytic = state.render.analyticParticles;
            if (MD3::Toggle("Analytic particles", &analytic)) {
                DispatchAndSave(controller, ParticleSaturn::App::SetAnalyticParticles{analytic}, callbacks);
            }
        }
        if (state.render.graphicsApi == ParticleSaturn::App::GraphicsApi::Metal) {
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
        const auto records = ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Snapshot();
        if (records.empty()) {
            ImGui::TextDisabled("No diagnostics");
        } else {
            const auto& record = records.back();
            const ImVec4 color = record.severity == ParticleSaturn::Services::Diagnostics::Severity::Error
                ? MD3::GetContext().colors.error : MD3::GetContext().colors.primary;
            ImGui::TextColored(color, "%s: %s", record.domain.c_str(), record.code.c_str());
            ImGui::TextWrapped("%s", record.message.c_str());
        }
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
