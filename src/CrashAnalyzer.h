#pragma once
// Crash Analyzer - PDB-based stack trace symbolizer
// Uses dynamic loading for DbgHelp to ensure compatibility across Windows versions

#include <Windows.h>

#include <imgui.h>

#include <algorithm>
#include <commdlg.h>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "Localization.h"
#include "md3/MD3.h"

#pragma comment(lib, "comdlg32.lib")

namespace CrashAnalyzer {

static void AppendTextToBuffer(char* dst, size_t dstSize, const std::string& text) {
    if (!dst || dstSize == 0 || text.empty()) {
        return;
    }

    size_t curLen = strlen(dst);
    if (curLen >= dstSize - 1) {
        return;
    }

    size_t maxAppend = (dstSize - 1) - curLen;
    size_t n         = std::min(maxAppend, text.size());
    memcpy(dst + curLen, text.data(), n);
    dst[curLen + n] = '\0';
}

// DbgHelp types (defined manually to avoid header dependency)
#define SYMOPT_UNDNAME 0x00000002
#define SYMOPT_DEFERRED_LOADS 0x00000004
#define SYMOPT_LOAD_LINES 0x00000010
#define SYMOPT_DEBUG 0x80000000
#define MAX_SYM_NAME 2000

typedef struct _SYMBOL_INFO {
    ULONG   SizeOfStruct;
    ULONG   TypeIndex;
    ULONG64 Reserved[2];
    ULONG   Index;
    ULONG   Size;
    ULONG64 ModBase;
    ULONG   Flags;
    ULONG64 Value;
    ULONG64 Address;
    ULONG   Register;
    ULONG   Scope;
    ULONG   Tag;
    ULONG   NameLen;
    ULONG   MaxNameLen;
    CHAR    Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;

typedef struct _IMAGEHLP_LINE64 {
    DWORD   SizeOfStruct;
    PVOID   Key;
    DWORD   LineNumber;
    PCHAR   FileName;
    DWORD64 Address;
} IMAGEHLP_LINE64, *PIMAGEHLP_LINE64;

// DbgHelp function pointers
typedef DWORD(WINAPI* PFN_SymSetOptions)(DWORD);
typedef BOOL(WINAPI* PFN_SymInitialize)(HANDLE, PCSTR, BOOL);
typedef BOOL(WINAPI* PFN_SymCleanup)(HANDLE);
typedef DWORD64(WINAPI* PFN_SymLoadModuleEx)(HANDLE, HANDLE, PCSTR, PCSTR, DWORD64, DWORD, PVOID, DWORD);
typedef BOOL(WINAPI* PFN_SymUnloadModule64)(HANDLE, DWORD64);
typedef BOOL(WINAPI* PFN_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL(WINAPI* PFN_SymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);

// DbgHelp dynamic loader
struct DbgHelpLoader {
    HMODULE     hModule     = nullptr;
    bool        initialized = false;
    bool        available   = false;
    std::string errorMessage;

    PFN_SymSetOptions        pSymSetOptions        = nullptr;
    PFN_SymInitialize        pSymInitialize        = nullptr;
    PFN_SymCleanup           pSymCleanup           = nullptr;
    PFN_SymLoadModuleEx      pSymLoadModuleEx      = nullptr;
    PFN_SymUnloadModule64    pSymUnloadModule64    = nullptr;
    PFN_SymFromAddr          pSymFromAddr          = nullptr;
    PFN_SymGetLineFromAddr64 pSymGetLineFromAddr64 = nullptr;

    bool Init() {
        if (initialized) {
            return available;
        }
        initialized = true;

        hModule = LoadLibraryA("dbghelp.dll");
        if (!hModule) {
            errorMessage = "DbgHelp.dll not found";
            return false;
        }

        pSymSetOptions        = (PFN_SymSetOptions)GetProcAddress(hModule, "SymSetOptions");
        pSymInitialize        = (PFN_SymInitialize)GetProcAddress(hModule, "SymInitialize");
        pSymCleanup           = (PFN_SymCleanup)GetProcAddress(hModule, "SymCleanup");
        pSymLoadModuleEx      = (PFN_SymLoadModuleEx)GetProcAddress(hModule, "SymLoadModuleEx");
        pSymUnloadModule64    = (PFN_SymUnloadModule64)GetProcAddress(hModule, "SymUnloadModule64");
        pSymFromAddr          = (PFN_SymFromAddr)GetProcAddress(hModule, "SymFromAddr");
        pSymGetLineFromAddr64 = (PFN_SymGetLineFromAddr64)GetProcAddress(hModule, "SymGetLineFromAddr64");

        if (!pSymSetOptions || !pSymInitialize || !pSymCleanup || !pSymLoadModuleEx || !pSymUnloadModule64 ||
            !pSymFromAddr) {
            errorMessage = "DbgHelp.dll version too old";
            FreeLibrary(hModule);
            hModule = nullptr;
            return false;
        }

        available = true;
        return true;
    }

    void Shutdown() {
        if (hModule) {
            FreeLibrary(hModule);
            hModule = nullptr;
        }
        available   = false;
        initialized = false;
    }
};

inline DbgHelpLoader g_dbgHelp;

// State
struct State {
    bool        windowOpen = false;
    bool        pdbLoaded  = false;
    std::string pdbPath;
    uint64_t    pdbSize            = 0;
    DWORD64     pdbBase            = 0;
    char        reportInput[16384] = {};
    std::string analysisResult;
    bool        hasResult = false;
};

inline State g_state;

// Get file size
inline uint64_t GetFileSize(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return 0;
    }
    return file.tellg();
}

// Format file size
inline std::string FormatFileSize(uint64_t bytes) {
    std::ostringstream oss;
    if (bytes >= 1024 * 1024) {
        oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0)) << " MB";
    } else if (bytes >= 1024) {
        oss << std::fixed << std::setprecision(1) << (bytes / 1024.0) << " KB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

// Open file dialog (wide char version for proper Unicode support)
inline std::string OpenPdbFileDialog() {
    const auto& str = i18n::Get();

    wchar_t filename[MAX_PATH] = {};

    // Convert title to wide string
    int          titleLen = MultiByteToWideChar(CP_UTF8, 0, str.selectPdbFile, -1, nullptr, 0);
    std::wstring wTitle(titleLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.selectPdbFile, -1, wTitle.data(), titleLen);

    OPENFILENAMEW ofn = {sizeof(ofn)};
    ofn.lpstrFilter   = L"PDB Files (*.pdb)\0*.pdb\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile     = filename;
    ofn.nMaxFile      = MAX_PATH;
    ofn.lpstrTitle    = wTitle.c_str();
    ofn.Flags         = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        // Convert wide string back to UTF-8
        int         len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, result.data(), len, nullptr, nullptr);
        return result;
    }
    return "";
}

// Load PDB file
inline bool LoadPdb(const std::string& path) {
    if (!g_dbgHelp.Init()) {
        return false;
    }

    if (g_state.pdbLoaded) {
        // Unload previous
        g_dbgHelp.pSymUnloadModule64(GetCurrentProcess(), g_state.pdbBase);
        g_dbgHelp.pSymCleanup(GetCurrentProcess());
        g_state.pdbLoaded = false;
    }

    HANDLE process = GetCurrentProcess();

    g_dbgHelp.pSymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_DEBUG);

    if (!g_dbgHelp.pSymInitialize(process, nullptr, FALSE)) {
        return false;
    }

    // Use a fake base address for the module
    DWORD64 fakeBase = 0x10000000;

    g_state.pdbBase = g_dbgHelp.pSymLoadModuleEx(process, nullptr, path.c_str(), nullptr, fakeBase, 0, nullptr, 0);
    if (g_state.pdbBase == 0) {
        g_dbgHelp.pSymCleanup(process);
        return false;
    }

    g_state.pdbLoaded = true;
    g_state.pdbPath   = path;
    g_state.pdbSize   = GetFileSize(path);

    return true;
}

// Extract addresses from crash report
inline std::vector<std::pair<std::string, uint64_t>> ExtractAddresses(const std::string& report) {
    std::vector<std::pair<std::string, uint64_t>> addresses;

    // Pattern: ModuleName.exe+0xOFFSET or ModuleName.dll+0xOFFSET
    std::regex addrRegex(R"((\w+\.(?:exe|dll))\+0x([0-9A-Fa-f]+))", std::regex::icase);

    std::sregex_iterator it(report.begin(), report.end(), addrRegex);
    std::sregex_iterator end;

    while (it != end) {
        std::smatch match     = *it;
        std::string offsetHex = match[2].str();

        uint64_t offset = std::stoull(offsetHex, nullptr, 16);
        addresses.push_back({match[0].str(), offset});

        ++it;
    }

    return addresses;
}

// Resolve address using loaded PDB
inline std::string ResolveAddress(uint64_t offset) {
    if (!g_state.pdbLoaded || !g_dbgHelp.available) {
        return "";
    }

    HANDLE  process = GetCurrentProcess();
    DWORD64 address = g_state.pdbBase + offset;

    std::ostringstream result;

    // Get symbol name
    char         symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol  = (PSYMBOL_INFO)symbolBuffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen   = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (g_dbgHelp.pSymFromAddr(process, address, &displacement, symbol)) {
        result << symbol->Name;

        if (displacement > 0) {
            result << "+0x" << std::hex << displacement << std::dec;
        }

        // Get line info (optional, may not be available)
        if (g_dbgHelp.pSymGetLineFromAddr64) {
            IMAGEHLP_LINE64 line             = {sizeof(line)};
            DWORD           lineDisplacement = 0;
            if (g_dbgHelp.pSymGetLineFromAddr64(process, address, &lineDisplacement, &line)) {
                // Extract just the filename, not full path
                const char* filename  = line.FileName;
                const char* lastSlash = strrchr(filename, '\\');
                if (lastSlash) {
                    filename = lastSlash + 1;
                }

                result << " [" << filename << ":" << line.LineNumber << "]";
            }
        }
    }

    return result.str();
}

// Analyze crash report
inline std::string Analyze(const std::string& report) {
    const auto& str = i18n::Get();

    if (!g_dbgHelp.Init()) {
        return g_dbgHelp.errorMessage;
    }

    if (!g_state.pdbLoaded) {
        return str.noPdbLoaded;
    }

    auto addresses = ExtractAddresses(report);

    if (addresses.empty()) {
        return str.analysisNoAddresses;
    }

    std::ostringstream result;
    result << str.analysisSuccess << "\n\n";

    int index = 0;
    for (const auto& [original, offset] : addresses) {
        result << "#" << index++ << "  " << original;

        std::string resolved = ResolveAddress(offset);
        if (!resolved.empty()) {
            result << "\n    -> " << resolved;
        }
        result << "\n\n";
    }

    return result.str();
}

// File drop callback handler - call from GLFW drop callback
inline void HandleFileDrop(const char* path) {
    if (!g_state.windowOpen) {
        return;
    }

    std::string pathStr(path);
    size_t      dotPos = pathStr.find_last_of('.');
    if (dotPos == std::string::npos) {
        return;
    }

    std::string ext = pathStr.substr(dotPos);

    // Convert to lowercase
    for (char& c : ext) {
        c = (char)tolower(c);
    }

    if (ext == ".pdb") {
        LoadPdb(pathStr);
    } else if (ext == ".txt" || ext == ".log") {
        // Load text file content into report input
        std::ifstream file(path);
        if (file) {
            std::ostringstream ss;
            ss << file.rdbuf();
            std::string content = ss.str();
            strncpy_s(g_state.reportInput, content.c_str(), sizeof(g_state.reportInput) - 1);
        }
    }
}

// Render crash analyzer window with optional blur background.
// blurTex 类型随后端而变：Diligent 传 ImTextureID（ITextureView*），OpenGL 传 GLuint。
#if defined(MD3_BACKEND_DILIGENT)
using CrashBlurTex = ImTextureID;
#else
using CrashBlurTex = unsigned int;
#endif
inline void Render(bool enableBlur = false, CrashBlurTex blurTex = 0, unsigned int scrWidth = 0,
                   unsigned int scrHeight = 0, bool isDarkMode = true) {
    if (!g_state.windowOpen) {
        return;
    }

    const auto& str = i18n::Get();

    // Set initial position to the right side of the screen to avoid overlap with debug panel
    ImGui::SetNextWindowPos(ImVec2(scrWidth > 600 ? (float)(scrWidth - 580) : 20.0f, 50.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(550, 750), ImGuiCond_FirstUseEver);

    ImGuiStyle& style            = ImGui::GetStyle();
    ImVec4      originalWindowBg = style.Colors[ImGuiCol_WindowBg];

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGrip, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, ImVec4(0, 0, 0, 0));

    if (ImGui::Begin(str.crashAnalyzerTitle, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize)) {
        ImVec2      pos  = ImGui::GetWindowPos();
        ImVec2      size = ImGui::GetWindowSize();
        ImDrawList* dl   = ImGui::GetWindowDrawList();

        if (enableBlur && blurTex != 0 && scrWidth > 0 && scrHeight > 0) {
#if defined(MD3_BACKEND_DILIGENT)
            // UV 计算：D3D12/Vulkan 坐标系（Y 从上到下，无需翻转）
            ImVec2 uv0 = ImVec2(pos.x / scrWidth, pos.y / scrHeight);
            ImVec2 uv1 = ImVec2((pos.x + size.x) / scrWidth, (pos.y + size.y) / scrHeight);
#else
            // UV 计算：OpenGL 坐标系（Y 从下到上，需翻转）
            ImVec2 uv0 = ImVec2(pos.x / scrWidth, 1.0f - pos.y / scrHeight);
            ImVec2 uv1 = ImVec2((pos.x + size.x) / scrWidth, 1.0f - (pos.y + size.y) / scrHeight);
#endif

            // blurTex 在 Diligent 版传入的是“已合成的 Acrylic 结果”
            MD3::AddImageRounded(dl, blurTex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1,
                                 IM_COL32(255, 255, 255, 255), style.WindowRounding);

            // 噪点层：防 banding + 增加“材质感”
            if (MD3::GetContext().noiseTextureID != 0) {
                float intensity = MD3::GetContext().noiseIntensity;
                if (intensity < 0.0f) {
                    intensity = 0.0f;
                }
                if (intensity > 0.1f) {
                    intensity = 0.1f;
                }
                int a = static_cast<int>(intensity * 255.0f + 0.5f);
                if (a < 0) {
                    a = 0;
                }
                if (a > 64) {
                    a = 64;
                }
                const ImU32 noiseCol = IM_COL32(255, 255, 255, a);
                MD3::AddImageRounded(dl, MD3::GetContext().noiseTextureID, pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                     uv0, uv1, noiseCol, style.WindowRounding);
            }

            ImU32 highlight = isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
        } else {
            ImVec4 bgCol = originalWindowBg;
            bgCol.w      = 0.95f;
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol),
                              style.WindowRounding);
        }

        // MD3 风格标题栏
        MD3::WindowTitleBarSpace();

        // Check DbgHelp availability
        if (!g_dbgHelp.Init()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s: %s", str.warningTitle,
                               g_dbgHelp.errorMessage.c_str());
            ImGui::TextWrapped("%s", str.symbolResolutionUnavailable);
            ImGui::Separator();
            ImGui::Spacing();
        }

        // PDB File section
        ImGui::Text("%s", str.pdbFile);
        ImGui::Separator();

        if (g_state.pdbLoaded) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.3f, 1.0f), "%s", str.pdbLoaded);
            ImGui::SameLine();

            // Show just filename
            std::string filename  = g_state.pdbPath;
            size_t      lastSlash = filename.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                filename = filename.substr(lastSlash + 1);
            }
            ImGui::Text("%s", filename.c_str());

            ImGui::Text("%s: %s", str.pdbSize, FormatFileSize(g_state.pdbSize).c_str());
        } else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", str.noPdbLoaded);
        }

        bool canLoadPdb = g_dbgHelp.available;
        if (!canLoadPdb) {
            ImGui::BeginDisabled();
        }
        if (MD3::TonalButton(str.dropOrSelect)) {
            std::string path = OpenPdbFileDialog();
            if (!path.empty()) {
                LoadPdb(path);
            }
        }
        if (!canLoadPdb) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Crash report input section
        ImGui::Text("%s", str.crashReport);
        ImGui::Separator();

        float dpi = MD3::GetContext().dpiScale;

        {
            ImGuiStyle& style = ImGui::GetStyle();

            float  width  = ImGui::GetContentRegionAvail().x;
            float  height = 200.0f;
            ImVec2 pos    = ImGui::GetCursorScreenPos();
            ImVec2 size   = ImVec2(width, height);

            // 使用适当的圆角和内边距
            float rounding = style.FrameRounding > 0.0f ? style.FrameRounding : 8.0f * dpi;
            // 内边距：水平方向略大于圆角半径的一半，防止文本触及圆角
            float padX = rounding * 0.5f + 2.0f * dpi;
            float padY = 4.0f * dpi;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, rounding);

            // 更新位置（因为样式可能影响布局）
            pos = ImGui::GetCursorScreenPos();

            // 裁剪区域在左右两侧内缩，防止滚动时文本超出圆角
            float  clipInset = rounding * 0.3f;
            ImVec2 clipMin(pos.x + clipInset, pos.y);
            ImVec2 clipMax(pos.x + size.x - clipInset, pos.y + size.y);
            MD3::PushRoundedClipRect(clipMin, clipMax, rounding - clipInset);
            ImGui::InputTextMultiline("##ReportInput", g_state.reportInput, sizeof(g_state.reportInput), size,
                                      ImGuiInputTextFlags_AllowTabInput);
            MD3::PopRoundedClipRect();

            ImGui::PopStyleVar(2);
        }

        // Right-click context menu for paste
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpi, 6.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
        if (ImGui::BeginPopupContextItem("##ReportInputContext")) {
            const char* clipText = ImGui::GetClipboardText();
            bool        canPaste = (clipText && clipText[0] != '\0');
            if (MD3::MenuItem(str.paste, canPaste, 36.0f * dpi)) {
                AppendTextToBuffer(g_state.reportInput, sizeof(g_state.reportInput), clipText);
            }
            bool canClear = g_state.reportInput[0] != '\0';
            if (MD3::MenuItem(str.clear, canClear, 36.0f * dpi)) {
                g_state.reportInput[0] = '\0';
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(2);

        ImGui::Spacing();

        // Analyze button
        bool canAnalyze = g_dbgHelp.available && g_state.pdbLoaded;
        if (!canAnalyze) {
            ImGui::BeginDisabled();
        }
        if (MD3::FilledButton(str.analyze, ImVec2(140, 0))) {
            g_state.analysisResult = Analyze(g_state.reportInput);
            g_state.hasResult      = true;
        }
        if (!canAnalyze) {
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        // Analysis result section
        if (g_state.hasResult) {
            ImGui::Text("%s", str.analysisResult);
            ImGui::Separator();

            ImGui::BeginChild("##ResultOutput", ImVec2(-1, 200), true);
            ImGui::TextUnformatted(g_state.analysisResult.c_str());
            ImGui::EndChild();

            if (MD3::TonalButton(str.copyResult)) {
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    // Convert UTF-8 to UTF-16 for proper Chinese character support
                    int wideLen = MultiByteToWideChar(CP_UTF8, 0, g_state.analysisResult.c_str(), -1, nullptr, 0);
                    if (wideLen > 0) {
                        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, wideLen * sizeof(wchar_t));
                        if (hMem) {
                            wchar_t* ptr = (wchar_t*)GlobalLock(hMem);
                            MultiByteToWideChar(CP_UTF8, 0, g_state.analysisResult.c_str(), -1, ptr, wideLen);
                            GlobalUnlock(hMem);
                            SetClipboardData(CF_UNICODETEXT, hMem);
                        }
                    }
                    CloseClipboard();
                }
            }
        }

        // 绘制 Ripple 效果
        MD3::DrawRipples();

        // 绘制 MD3 滚动条
        MD3::WindowScrollbar(40.0f * ImGui::GetIO().FontGlobalScale);

        // 处理窗口 resize（自定义实现）
        MD3::WindowResize(400.0f, 300.0f);

        // 绘制标题栏（在所有内容之上）
        MD3::WindowTitleBar(str.crashAnalyzerTitle, &g_state.windowOpen);
    }
    ImGui::End();
    ImGui::PopStyleColor(4);
}

// Open the analyzer window
inline void Open() {
    g_state.windowOpen = true;
}

// Check if window is open
inline bool IsOpen() {
    return g_state.windowOpen;
}

// Cleanup
inline void Shutdown() {
    if (g_state.pdbLoaded && g_dbgHelp.available) {
        g_dbgHelp.pSymUnloadModule64(GetCurrentProcess(), g_state.pdbBase);
        g_dbgHelp.pSymCleanup(GetCurrentProcess());
        g_state.pdbLoaded = false;
    }
    g_dbgHelp.Shutdown();
}

} // namespace CrashAnalyzer
