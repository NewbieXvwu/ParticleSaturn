// UIManager.cpp - UI 管理器实现
// 将大型函数从头文件移到 cpp 文件，减少编译时间

#include "pch.h"

#include "UIManager.h"

// 全局变量定义
std::unordered_map<ImGuiID, UIAnimState> g_animStates;

// 静态 AppState 指针，用于需要访问状态的内部函数
static AppState* s_appState = nullptr;

namespace UIManager {

// 应用 Material You 主题
void ApplyMaterialYouTheme(bool dark) {
    ImGuiStyle& style  = ImGui::GetStyle();
    ImVec4*     colors = style.Colors;

    style.WindowRounding    = 28.0f;  // MD3 大圆角
    style.ChildRounding     = 16.0f;
    style.FrameRounding     = 20.0f;
    style.PopupRounding     = 20.0f;  // Popup 也用大圆角
    style.ScrollbarRounding = 12.0f;
    style.GrabRounding      = 20.0f;
    style.WindowPadding     = ImVec2(20, 20);
    style.FramePadding      = ImVec2(10, 6);
    style.ItemSpacing       = ImVec2(10, 12);
    style.WindowBorderSize  = 0.0f;

    if (dark) {
        ImVec4 surface     = ImVec4(0.12f, 0.12f, 0.14f, 0.05f);
        ImVec4 cardBg      = ImVec4(0.18f, 0.18f, 0.20f, 0.50f);
        ImVec4 buttonBg    = ImVec4(0.22f, 0.22f, 0.24f, 1.00f);
        ImVec4 buttonHover = ImVec4(0.28f, 0.28f, 0.30f, 1.00f);
        ImVec4 primary     = ImVec4(0.651f, 0.851f, 1.00f, 1.00f);
        ImVec4 primaryDim  = ImVec4(0.35f, 0.55f, 0.75f, 1.00f);
        ImVec4 text        = ImVec4(0.92f, 0.92f, 0.95f, 1.00f);
        ImVec4 textDim     = ImVec4(0.70f, 0.70f, 0.75f, 1.00f);
        ImVec4 outline     = ImVec4(0.50f, 0.50f, 0.55f, 0.40f);

        colors[ImGuiCol_WindowBg]             = surface;
        colors[ImGuiCol_ChildBg]              = cardBg;
        colors[ImGuiCol_PopupBg]              = ImVec4(0.15f, 0.15f, 0.17f, 0.98f);
        colors[ImGuiCol_Border]               = outline;
        colors[ImGuiCol_FrameBg]              = buttonBg;
        colors[ImGuiCol_FrameBgHovered]       = buttonHover;
        colors[ImGuiCol_FrameBgActive]        = ImVec4(0.32f, 0.32f, 0.35f, 1.00f);
        colors[ImGuiCol_TitleBg]              = cardBg;
        colors[ImGuiCol_TitleBgActive]        = cardBg;
        colors[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_ScrollbarGrab]        = outline;
        colors[ImGuiCol_ScrollbarGrabHovered] = textDim;
        colors[ImGuiCol_ScrollbarGrabActive]  = text;
        colors[ImGuiCol_CheckMark]            = primary;
        colors[ImGuiCol_SliderGrab]           = primary;
        colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.8f, 0.9f, 1.0f, 1.0f);
        colors[ImGuiCol_Button]               = buttonBg;
        colors[ImGuiCol_ButtonHovered]        = buttonHover;
        colors[ImGuiCol_ButtonActive]         = primaryDim;
        colors[ImGuiCol_Text]                 = text;
        colors[ImGuiCol_TextDisabled]         = textDim;
        colors[ImGuiCol_Separator]            = outline;
        colors[ImGuiCol_Header]               = buttonBg;
        colors[ImGuiCol_HeaderHovered]        = buttonHover;
        colors[ImGuiCol_HeaderActive]         = primaryDim;
    } else {
        ImVec4 surface      = ImVec4(0.98f, 0.98f, 0.98f, 0.05f);
        ImVec4 surfaceVar   = ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
        ImVec4 primary      = ImVec4(0.00f, 0.35f, 0.65f, 1.00f);
        ImVec4 onSurface    = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
        ImVec4 onSurfaceVar = ImVec4(0.40f, 0.40f, 0.45f, 1.00f);
        ImVec4 outline      = ImVec4(0.50f, 0.50f, 0.50f, 0.20f);

        colors[ImGuiCol_WindowBg]         = surface;
        colors[ImGuiCol_ChildBg]          = surfaceVar;
        colors[ImGuiCol_PopupBg]          = ImVec4(1.00f, 1.00f, 1.00f, 0.98f);
        colors[ImGuiCol_Border]           = outline;
        colors[ImGuiCol_FrameBg]          = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_FrameBgActive]    = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_CheckMark]        = primary;
        colors[ImGuiCol_SliderGrab]       = primary;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.2f, 0.5f, 0.8f, 1.0f);
        colors[ImGuiCol_Button]           = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_ButtonHovered]    = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_ButtonActive]     = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_Text]             = onSurface;
        colors[ImGuiCol_TextDisabled]     = onSurfaceVar;
        colors[ImGuiCol_Separator]        = outline;
        colors[ImGuiCol_Header]           = ImVec4(0.0f, 0.0f, 0.0f, 0.05f);
        colors[ImGuiCol_HeaderHovered]    = ImVec4(0.0f, 0.0f, 0.0f, 0.08f);
        colors[ImGuiCol_HeaderActive]     = ImVec4(0.0f, 0.0f, 0.0f, 0.12f);
        colors[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TitleBg]          = surfaceVar;
        colors[ImGuiCol_TitleBgActive]    = surfaceVar;
    }
}

// Material Design 3 风格开关控件
bool ToggleMD3(const char* label, bool* v, float dt) {
    using namespace ImGui;
    ImGuiID id = GetID(label);
    if (g_animStates.find(id) == g_animStates.end()) {
        g_animStates[id].active        = *v;
        g_animStates[id].bgOpacity.val = *v ? 1.0f : 0.0f;
        g_animStates[id].knobPos.val   = *v ? 1.0f : 0.0f;
    }
    UIAnimState& s     = g_animStates[id];
    s.bgOpacity.target = *v ? 1.0f : 0.0f;
    s.knobPos.target   = *v ? 1.0f : 0.0f;
    bool hovered       = IsItemHovered();
    s.knobSize.target  = (*v || hovered) ? 1.0f : 0.0f;
    s.bgOpacity.Update(dt, 18.0f);
    s.knobPos.Update(dt, 14.0f);
    s.knobSize.Update(dt, 20.0f);

    float       height = 28.0f * (s_appState ? s_appState->ui.dpiScale : 1.0f);
    float       width  = 52.0f * (s_appState ? s_appState->ui.dpiScale : 1.0f);
    ImVec2      p      = GetCursorScreenPos();
    ImDrawList* dl     = GetWindowDrawList();

    bool pressed = InvisibleButton(label, ImVec2(width, height));
    if (pressed) {
        *v = !*v;
    }

    ImVec4 cTrackOff = GetStyle().Colors[ImGuiCol_FrameBg];
    ImVec4 cTrackOn  = GetStyle().Colors[ImGuiCol_CheckMark];
    ImVec4 cThumbOff = GetStyle().Colors[ImGuiCol_TextDisabled];
    ImVec4 cThumbOn  = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (GetStyle().Colors[ImGuiCol_WindowBg].x > 0.5f) {
        cThumbOn = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float  t      = s.bgOpacity.val;
    ImVec4 cTrack = ImVec4(cTrackOff.x + (cTrackOn.x - cTrackOff.x) * t, cTrackOff.y + (cTrackOn.y - cTrackOff.y) * t,
                           cTrackOff.z + (cTrackOn.z - cTrackOff.z) * t, cTrackOff.w + (cTrackOn.w - cTrackOff.w) * t);

    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), GetColorU32(cTrack), height * 0.5f);
    if (t < 0.95f) {
        ImU32 borderColor = GetColorU32(ImGuiCol_Border);
        dl->AddRect(p, ImVec2(p.x + width, p.y + height), borderColor, height * 0.5f, 0, 1.0f);
    }

    float r_normal = height * 0.25f;
    float r_active = height * 0.38f;
    float r_cur    = r_normal + (r_active - r_normal) * s.knobSize.val;
    float pad      = height * 0.15f;
    float x_start  = p.x + pad + r_active;
    float x_end    = p.x + width - pad - r_active;
    float x_cur    = x_start + (x_end - x_start) * s.knobPos.val;

    ImU32 colThumb = GetColorU32((s.knobPos.val > 0.5f) ? cThumbOn : cThumbOff);
    dl->AddCircleFilled(ImVec2(x_cur, p.y + height * 0.5f), r_cur, colThumb);

    SameLine();
    SetCursorPosX(GetCursorPosX() + 10.0f);
    // 垂直居中对齐文本
    float textHeight = GetTextLineHeight();
    float offsetY    = (height - textHeight) * 0.5f;
    SetCursorPosY(GetCursorPosY() + offsetY);
    Text("%s", label);
    return pressed;
}

// 初始化 ImGui
void Init(GLFWwindow* window, AppState& state) {
    s_appState = &state; // 保存状态指针

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    // 高 DPI 缩放
    float xscale, yscale;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    state.ui.dpiScale = (xscale > yscale) ? xscale : yscale;
    if (state.ui.dpiScale < 1.0f) {
        state.ui.dpiScale = 1.0f;
    }

    // 加载系统字体
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    float fontSize      = 16.0f * state.ui.dpiScale;

    // English fonts (primary) - fallback order
    const char* englishFonts[] = {"C:\\Windows\\Fonts\\CascadiaCode.ttf", "C:\\Windows\\Fonts\\CascadiaMono.ttf",
                                  "C:\\Windows\\Fonts\\consola.ttf", "C:\\Windows\\Fonts\\arial.ttf", nullptr};

    // Chinese fonts (merged) - fallback order: DengXian → YaHei Light → YaHei → SimHei
    const char* chineseFonts[] = {"C:\\Windows\\Fonts\\Deng.ttf",   // DengXian (Win10+)
                                  "C:\\Windows\\Fonts\\msyhl.ttc",  // Microsoft YaHei Light
                                  "C:\\Windows\\Fonts\\msyh.ttc",   // Microsoft YaHei
                                  "C:\\Windows\\Fonts\\simhei.ttf", // SimHei
                                  nullptr};

    // Load primary English font
    ImFont* font = nullptr;
    for (int i = 0; englishFonts[i] && !font; i++) {
        font = io.Fonts->AddFontFromFileTTF(englishFonts[i], fontSize, &fontCfg);
        if (font) {
            std::cout << "[UI] Primary font: " << englishFonts[i] << std::endl;
        }
    }

    if (!font) {
        fontCfg.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&fontCfg);
        std::cout << "[UI] Using default font" << std::endl;
    }

    // Merge Chinese font for CJK characters
    fontCfg.MergeMode  = true;
    bool chineseLoaded = false;
    for (int i = 0; chineseFonts[i] && !chineseLoaded; i++) {
        ImFont* cjkFont =
            io.Fonts->AddFontFromFileTTF(chineseFonts[i], fontSize, &fontCfg, io.Fonts->GetGlyphRangesChineseFull());
        if (cjkFont) {
            std::cout << "[UI] Chinese font: " << chineseFonts[i] << std::endl;
            chineseLoaded = true;
        }
    }
    if (!chineseLoaded) {
        std::cout << "[UI] Warning: No Chinese font loaded" << std::endl;
    }

    ApplyMaterialYouTheme(state.ui.isDarkMode);

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(state.ui.dpiScale);
    std::cout << "[Main] DPI scale: " << state.ui.dpiScale << std::endl;
    std::cout << "[Main] Theme: " << (state.ui.isDarkMode ? "Dark" : "Light") << std::endl;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 430");
    std::cout << "[Main] Dear ImGui initialized." << std::endl;
}

// 关闭 ImGui
void Shutdown() {
    s_appState = nullptr;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

} // namespace UIManager

// 主题变更回调
#ifdef _WIN32
namespace WindowManager {
void OnThemeChanged(bool isDark) {
    UIManager::ApplyMaterialYouTheme(isDark);
}
} // namespace WindowManager
#endif
