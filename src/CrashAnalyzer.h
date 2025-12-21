#pragma once
// Crash Analyzer - PDB-based stack trace symbolizer
// Uses dynamic loading for DbgHelp to ensure compatibility across Windows versions

#include <Windows.h>

#include <commdlg.h>
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

// Render crash analyzer window with optional blur background
inline void Render(bool enableBlur = false, unsigned int blurTex = 0, unsigned int scrWidth = 0,
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

    if (ImGui::Begin(str.crashAnalyzerTitle, &g_state.windowOpen)) {
        ImVec2      pos  = ImGui::GetWindowPos();
        ImVec2      size = ImGui::GetWindowSize();
        ImDrawList* dl   = ImGui::GetWindowDrawList();

        if (enableBlur && blurTex != 0 && scrWidth > 0 && scrHeight > 0) {
            ImVec2 uv0 = ImVec2(pos.x / scrWidth, 1.0f - pos.y / scrHeight);
            ImVec2 uv1 = ImVec2((pos.x + size.x) / scrWidth, 1.0f - (pos.y + size.y) / scrHeight);
            dl->AddImage((ImTextureID)(intptr_t)blurTex, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1);
            ImU32 tintColor = isDarkMode ? IM_COL32(20, 20, 25, 180) : IM_COL32(245, 245, 255, 150);
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tintColor, style.WindowRounding);
            ImU32 highlight = isDarkMode ? IM_COL32(255, 255, 255, 40) : IM_COL32(255, 255, 255, 120);
            dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), highlight, style.WindowRounding, 0, 1.0f);
        } else {
            ImVec4 bgCol = originalWindowBg;
            bgCol.w      = 0.95f;
            dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), ImGui::GetColorU32(bgCol),
                              style.WindowRounding);
        }
        // Check DbgHelp availability
        if (!g_dbgHelp.Init()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Warning: %s", g_dbgHelp.errorMessage.c_str());
            ImGui::TextWrapped("Symbol resolution unavailable. You can still view addresses but cannot resolve them to "
                               "function names.");
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

        ImGui::InputTextMultiline("##ReportInput", g_state.reportInput, sizeof(g_state.reportInput), ImVec2(-1, 200),
                                  ImGuiInputTextFlags_AllowTabInput);

        // Right-click context menu for paste
        if (ImGui::BeginPopupContextItem("##ReportInputContext")) {
            if (ImGui::MenuItem(str.paste)) {
                if (OpenClipboard(nullptr)) {
                    HANDLE hData = GetClipboardData(CF_TEXT);
                    if (hData) {
                        char* pszText = static_cast<char*>(GlobalLock(hData));
                        if (pszText) {
                            size_t currentLen = strlen(g_state.reportInput);
                            size_t pasteLen   = strlen(pszText);
                            size_t maxLen     = sizeof(g_state.reportInput) - 1;
                            if (currentLen + pasteLen < maxLen) {
                                strcat_s(g_state.reportInput, pszText);
                            }
                            GlobalUnlock(hData);
                        }
                    }
                    CloseClipboard();
                }
            }
            if (ImGui::MenuItem(str.clear)) {
                g_state.reportInput[0] = '\0';
            }
            ImGui::EndPopup();
        }

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
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, g_state.analysisResult.size() + 1);
                    if (hMem) {
                        char* ptr = (char*)GlobalLock(hMem);
                        memcpy(ptr, g_state.analysisResult.c_str(), g_state.analysisResult.size() + 1);
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_TEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
        }

        // 绘制 Ripple 效果
        MD3::DrawRipples();
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
