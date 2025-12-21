#pragma once
// Error Handler - crash capture, system info collection, error dialogs

#include <Windows.h>

#include <CommCtrl.h>
#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

#include "DebugLog.h"
#include "Localization.h"

// Note: comctl32.lib is NOT linked statically to avoid ordinal 345 error
// TaskDialogIndirect and InitCommonControlsEx are loaded dynamically at runtime

namespace ErrorHandler {

// Application execution stages
enum class AppStage {
    STARTUP,
    WINDOW_INIT,
    OPENGL_INIT,
    SHADER_COMPILE,
    CAMERA_INIT,
    HAND_TRACKER_INIT,
    PARTICLE_INIT,
    IMGUI_INIT,
    RENDER_LOOP,
    SHUTDOWN
};

// Global state
inline AppStage                              g_currentStage = AppStage::STARTUP;
inline std::chrono::steady_clock::time_point g_startTime;
inline int                                   g_frameCount         = 0;
inline int                                   g_particleCount      = 0;
inline float                                 g_currentLOD         = 1.0f;
inline bool                                  g_handTrackingActive = false;
inline std::string                           g_cameraDevice       = "";
inline int                                   g_cameraIndex        = -1;
inline int                                   g_cameraWidth        = 0;
inline int                                   g_cameraHeight       = 0;
inline bool                                  g_cameraActive       = false;

// Pending error for ImGui display
struct PendingError {
    bool        active    = false;
    bool        isWarning = true;
    std::string title;
    std::string message;
    std::string details;
    bool        detailsExpanded = false;
};

inline PendingError g_pendingError;

// Stage name conversion
inline const char* GetStageName(AppStage stage) {
    switch (stage) {
    case AppStage::STARTUP:
        return "STARTUP";
    case AppStage::WINDOW_INIT:
        return "WINDOW_INIT";
    case AppStage::OPENGL_INIT:
        return "OPENGL_INIT";
    case AppStage::SHADER_COMPILE:
        return "SHADER_COMPILE";
    case AppStage::CAMERA_INIT:
        return "CAMERA_INIT";
    case AppStage::HAND_TRACKER_INIT:
        return "HAND_TRACKER_INIT";
    case AppStage::PARTICLE_INIT:
        return "PARTICLE_INIT";
    case AppStage::IMGUI_INIT:
        return "IMGUI_INIT";
    case AppStage::RENDER_LOOP:
        return "RENDER_LOOP";
    case AppStage::SHUTDOWN:
        return "SHUTDOWN";
    default:
        return "UNKNOWN";
    }
}

// Exception code to string
inline std::string GetExceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "Access Violation (0xC0000005)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        return "Array Bounds Exceeded (0xC000008C)";
    case EXCEPTION_BREAKPOINT:
        return "Breakpoint (0x80000003)";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "Datatype Misalignment (0x80000002)";
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return "Float Denormal Operand (0xC000008D)";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "Float Divide by Zero (0xC000008E)";
    case EXCEPTION_FLT_INEXACT_RESULT:
        return "Float Inexact Result (0xC000008F)";
    case EXCEPTION_FLT_INVALID_OPERATION:
        return "Float Invalid Operation (0xC0000090)";
    case EXCEPTION_FLT_OVERFLOW:
        return "Float Overflow (0xC0000091)";
    case EXCEPTION_FLT_STACK_CHECK:
        return "Float Stack Check (0xC0000092)";
    case EXCEPTION_FLT_UNDERFLOW:
        return "Float Underflow (0xC0000093)";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "Illegal Instruction (0xC000001D)";
    case EXCEPTION_IN_PAGE_ERROR:
        return "In Page Error (0xC0000006)";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "Integer Divide by Zero (0xC0000094)";
    case EXCEPTION_INT_OVERFLOW:
        return "Integer Overflow (0xC0000095)";
    case EXCEPTION_INVALID_DISPOSITION:
        return "Invalid Disposition (0xC0000026)";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
        return "Noncontinuable Exception (0xC0000025)";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "Privileged Instruction (0xC0000096)";
    case EXCEPTION_SINGLE_STEP:
        return "Single Step (0x80000004)";
    case EXCEPTION_STACK_OVERFLOW:
        return "Stack Overflow (0xC00000FD)";
    default: {
        std::ostringstream oss;
        oss << "Unknown Exception (0x" << std::hex << std::uppercase << code << ")";
        return oss.str();
    }
    }
}

// Get friendly error message for exception
inline const char* GetFriendlyMessage(DWORD code) {
    const auto& str = i18n::Get();
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return str.accessViolation;
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return str.calculationError;
    case EXCEPTION_STACK_OVERFLOW:
        return str.stackOverflow;
    default:
        return str.unexpectedError;
    }
}

// Format uptime
inline std::string FormatUptime() {
    auto               now     = std::chrono::steady_clock::now();
    auto               elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - g_startTime).count();
    int                hours   = static_cast<int>(elapsed / 3600);
    int                minutes = static_cast<int>((elapsed % 3600) / 60);
    int                seconds = static_cast<int>(elapsed % 60);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << hours << ":" << std::setfill('0') << std::setw(2) << minutes << ":"
        << std::setfill('0') << std::setw(2) << seconds;
    return oss.str();
}

// Get Windows version string
inline std::string GetWindowsVersion() {
    OSVERSIONINFOEXW osvi = {sizeof(osvi)};
    // RtlGetVersion returns NTSTATUS (which is LONG)
    typedef LONG(WINAPI * RtlGetVersionFunc)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        RtlGetVersionFunc RtlGetVersion = (RtlGetVersionFunc)GetProcAddress(ntdll, "RtlGetVersion");
        if (RtlGetVersion) {
            RtlGetVersion((PRTL_OSVERSIONINFOW)&osvi);
        }
    }
    std::ostringstream oss;
    oss << "Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << " (Build " << osvi.dwBuildNumber << ")";
    return oss.str();
}

// Get system language
inline std::string GetSystemLanguage() {
    wchar_t locale[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) {
        char buf[LOCALE_NAME_MAX_LENGTH];
        WideCharToMultiByte(CP_UTF8, 0, locale, -1, buf, sizeof(buf), nullptr, nullptr);
        return buf;
    }
    return "Unknown";
}

// Get memory info
inline std::string GetMemoryInfo() {
    MEMORYSTATUSEX memInfo = {sizeof(memInfo)};
    GlobalMemoryStatusEx(&memInfo);
    double             usedGB  = (memInfo.ullTotalPhys - memInfo.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    double             totalGB = memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << usedGB << " GB / " << totalGB << " GB";
    return oss.str();
}

// GPU info storage (set from Main.cpp using OpenGL info)
inline std::string g_gpuRenderer;
inline std::string g_gpuVersion;

// Set GPU info (call after OpenGL init)
inline void SetGPUInfo(const std::string& renderer, const std::string& version) {
    g_gpuRenderer = renderer;
    g_gpuVersion  = version;
}

// Capture call stack using CaptureStackBackTrace (no DbgHelp dependency)
inline std::vector<std::string> CaptureCallStack(CONTEXT* context = nullptr, int maxFrames = 20) {
    std::vector<std::string> stack;

    // Use CaptureStackBackTrace for simple stack capture (works without DbgHelp)
    void* frames[64];
    int   frameCount = maxFrames > 64 ? 64 : maxFrames;

    // If context is provided (from exception), we can only get limited info
    // CaptureStackBackTrace only works for current thread's stack
    USHORT captured = CaptureStackBackTrace(0, (DWORD)frameCount, frames, nullptr);

    for (USHORT i = 0; i < captured; i++) {
        std::ostringstream oss;
        oss << "#" << i << "  ";

        HMODULE module               = nullptr;
        char    moduleName[MAX_PATH] = "Unknown";

        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)frames[i], &module)) {
            GetModuleFileNameA(module, moduleName, MAX_PATH);
            char* lastSlash = strrchr(moduleName, '\\');
            if (lastSlash) {
                memmove(moduleName, lastSlash + 1, strlen(lastSlash));
            }

            // Calculate offset from module base
            uintptr_t offset = (uintptr_t)frames[i] - (uintptr_t)module;
            oss << moduleName << "+0x" << std::hex << std::uppercase << offset;
        } else {
            oss << "0x" << std::hex << std::uppercase << (uintptr_t)frames[i];
        }

        stack.push_back(oss.str());
    }

    return stack;
}

// Build detailed crash report
inline std::string BuildCrashReport(EXCEPTION_RECORD* exceptionRecord = nullptr, CONTEXT* context = nullptr) {
    const auto&        str = i18n::Get();
    std::ostringstream report;

    // Exception info
    report << "== " << str.sectionException << " ==\n";
    if (exceptionRecord) {
        report << str.fieldType << ": " << GetExceptionName(exceptionRecord->ExceptionCode) << "\n";
        report << str.fieldAddress << ": 0x" << std::hex << std::uppercase
               << (uintptr_t)exceptionRecord->ExceptionAddress << std::dec << "\n";

        if (exceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && exceptionRecord->NumberParameters >= 2) {
            const char* op = (exceptionRecord->ExceptionInformation[0] == 0) ? str.statusRead : str.statusWrite;
            report << str.fieldOperation << ": " << op << " 0x" << std::hex << std::uppercase
                   << exceptionRecord->ExceptionInformation[1] << std::dec << "\n";
        }
    }
    report << str.fieldStage << ": " << GetStageName(g_currentStage) << "\n\n";

    // Call stack
    report << "== " << str.sectionCallStack << " ==\n";
    auto stack = CaptureCallStack(context);
    for (const auto& frame : stack) {
        report << frame << "\n";
    }
    report << "\n";

    // System info
    report << "== " << str.sectionSystem << " ==\n";
    report << str.fieldOS << ": " << GetWindowsVersion() << "\n";
    report << str.fieldLanguage << ": " << GetSystemLanguage() << "\n";
    report << str.fieldMemory << ": " << GetMemoryInfo() << "\n\n";

    // Graphics info
    report << "== " << str.sectionGraphics << " ==\n";
    if (!g_gpuRenderer.empty()) {
        report << str.fieldGPU << ": " << g_gpuRenderer << "\n";
    }
    if (!g_gpuVersion.empty()) {
        report << str.fieldOpenGL << ": " << g_gpuVersion << "\n";
    }
    report << "\n";

    // Camera info
    report << "== " << str.sectionCamera << " ==\n";
    if (g_cameraIndex >= 0) {
        report << str.fieldCameraDevice << ": " << (g_cameraDevice.empty() ? str.statusUnknown : g_cameraDevice)
               << "\n";
        report << str.fieldCameraIndex << ": " << g_cameraIndex << "\n";
        if (g_cameraWidth > 0 && g_cameraHeight > 0) {
            report << str.fieldCameraResolution << ": " << g_cameraWidth << "x" << g_cameraHeight << "\n";
        }
        report << str.fieldCameraStatus << ": " << (g_cameraActive ? str.statusActive : str.statusInactive) << "\n";
    } else {
        report << str.fieldCameraStatus << ": " << str.statusDisabled << "\n";
    }
    report << "\n";

    // App state
    report << "== " << str.sectionAppState << " ==\n";
    report << str.fieldVersion << ": " << i18n::GetVersion() << "\n";
    report << str.fieldUptime << ": " << FormatUptime() << "\n";
    report << str.fieldFrame << ": " << g_frameCount << "\n";
    report << str.fieldParticles << ": " << g_particleCount << "\n";
    report << str.fieldLOD << ": " << std::fixed << std::setprecision(2) << g_currentLOD << "\n";
    report << str.fieldHandTracking << ": " << (g_handTrackingActive ? str.statusActive : str.statusInactive) << "\n\n";

    // Recent logs
    report << "== " << str.sectionRecentLogs << " ==\n";
    std::string logs = DebugLog::Instance().GetAllText();
    // Get last 10 lines
    std::vector<std::string> lines;
    std::istringstream       iss(logs);
    std::string              line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    size_t start = lines.size() > 10 ? lines.size() - 10 : 0;
    for (size_t i = start; i < lines.size(); i++) {
        report << lines[i] << "\n";
    }

    return report.str();
}

// Copy text to clipboard
inline void CopyToClipboard(const std::string& text) {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            char* ptr = (char*)GlobalLock(hMem);
            memcpy(ptr, text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
}

// Show fatal crash dialog - fallback to MessageBox if TaskDialog unavailable
inline void ShowFatalCrashDialog(EXCEPTION_RECORD* exceptionRecord, CONTEXT* context) {
    const auto& str = i18n::Get();

    std::string friendlyMsg = GetFriendlyMessage(exceptionRecord->ExceptionCode);
    std::string details     = BuildCrashReport(exceptionRecord, context);

    // Try to use TaskDialogIndirect dynamically (requires comctl32 v6.0)
    typedef HRESULT(WINAPI * TaskDialogIndirectFunc)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE                hComctl             = GetModuleHandleW(L"comctl32.dll");
    TaskDialogIndirectFunc pTaskDialogIndirect = nullptr;
    if (hComctl) {
        pTaskDialogIndirect = (TaskDialogIndirectFunc)GetProcAddress(hComctl, "TaskDialogIndirect");
    }

    if (pTaskDialogIndirect) {
        // Convert to wide strings
        int titleLen    = MultiByteToWideChar(CP_UTF8, 0, str.crashTitle, -1, nullptr, 0);
        int msgLen      = MultiByteToWideChar(CP_UTF8, 0, friendlyMsg.c_str(), -1, nullptr, 0);
        int expandLen   = MultiByteToWideChar(CP_UTF8, 0, str.expandDetails, -1, nullptr, 0);
        int collapseLen = MultiByteToWideChar(CP_UTF8, 0, str.collapseDetails, -1, nullptr, 0);
        int copyLen     = MultiByteToWideChar(CP_UTF8, 0, str.copyAll, -1, nullptr, 0);
        int closeLen    = MultiByteToWideChar(CP_UTF8, 0, str.closeProgram, -1, nullptr, 0);
        int detailsLen  = MultiByteToWideChar(CP_UTF8, 0, details.c_str(), -1, nullptr, 0);

        std::wstring wTitle(titleLen, 0);
        std::wstring wMsg(msgLen, 0);
        std::wstring wExpand(expandLen, 0);
        std::wstring wCollapse(collapseLen, 0);
        std::wstring wCopy(copyLen, 0);
        std::wstring wClose(closeLen, 0);
        std::wstring wDetails(detailsLen, 0);

        MultiByteToWideChar(CP_UTF8, 0, str.crashTitle, -1, wTitle.data(), titleLen);
        MultiByteToWideChar(CP_UTF8, 0, friendlyMsg.c_str(), -1, wMsg.data(), msgLen);
        MultiByteToWideChar(CP_UTF8, 0, str.expandDetails, -1, wExpand.data(), expandLen);
        MultiByteToWideChar(CP_UTF8, 0, str.collapseDetails, -1, wCollapse.data(), collapseLen);
        MultiByteToWideChar(CP_UTF8, 0, str.copyAll, -1, wCopy.data(), copyLen);
        MultiByteToWideChar(CP_UTF8, 0, str.closeProgram, -1, wClose.data(), closeLen);
        MultiByteToWideChar(CP_UTF8, 0, details.c_str(), -1, wDetails.data(), detailsLen);

        // Store details for copy button callback
        static std::string s_details;
        s_details = details;

        TASKDIALOGCONFIG config        = {sizeof(config)};
        config.dwFlags                 = TDF_ENABLE_HYPERLINKS | TDF_EXPAND_FOOTER_AREA | TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons         = 0;
        config.pszWindowTitle          = L"Particle Saturn";
        config.pszMainIcon             = TD_ERROR_ICON;
        config.pszMainInstruction      = wTitle.c_str();
        config.pszContent              = wMsg.c_str();
        config.pszExpandedInformation  = wDetails.c_str();
        config.pszExpandedControlText  = wCollapse.c_str();
        config.pszCollapsedControlText = wExpand.c_str();

        TASKDIALOG_BUTTON buttons[] = {{1001, wCopy.c_str()}, {1002, wClose.c_str()}};
        config.pButtons             = buttons;
        config.cButtons             = 2;
        config.nDefaultButton       = 1002;

        config.pfCallback = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR lpRefData) -> HRESULT {
            if (msg == TDN_BUTTON_CLICKED && wParam == 1001) {
                CopyToClipboard(s_details);
                return S_FALSE; // Don't close dialog
            }
            return S_OK;
        };

        pTaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    } else {
        // Fallback to simple MessageBox
        std::string fullMsg = friendlyMsg + "\n\n" + details;
        MessageBoxA(nullptr, fullMsg.c_str(), "Particle Saturn - Crash", MB_OK | MB_ICONERROR);
    }
}

// Show recoverable error (ImGui modal) - call from render loop
inline void ShowRecoverableError(const char* title, const char* message, const char* details, bool isWarning = true) {
    g_pendingError.active          = true;
    g_pendingError.isWarning       = isWarning;
    g_pendingError.title           = title;
    g_pendingError.message         = message;
    g_pendingError.details         = details;
    g_pendingError.detailsExpanded = false;
}

// Show recoverable error with localized message
inline void ShowError(const char* localizedMessage, const std::string& technicalDetails = "") {
    const auto& str = i18n::Get();
    ShowRecoverableError(str.errorTitle, localizedMessage, technicalDetails.c_str(), false);
}

inline void ShowWarning(const char* localizedMessage, const std::string& technicalDetails = "") {
    const auto& str = i18n::Get();
    ShowRecoverableError(str.warningTitle, localizedMessage, technicalDetails.c_str(), true);
}

// Render error dialog (call from main loop after ImGui::NewFrame)
inline void RenderErrorDialog(float dt) {
    if (!g_pendingError.active) {
        return;
    }

    const auto& str = i18n::Get();

    ImGui::OpenPopup("##ErrorDialog");

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(450, 0), ImGuiCond_Appearing);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;

    if (ImGui::BeginPopupModal("##ErrorDialog", nullptr, flags)) {
        // Icon and title
        ImVec4 iconColor = g_pendingError.isWarning ? ImVec4(1.0f, 0.7f, 0.0f, 1.0f)  // Orange for warning
                                                    : ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // Red for error

        ImGui::PushStyleColor(ImGuiCol_Text, iconColor);
        ImGui::Text(g_pendingError.isWarning ? "!" : "X");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Text("%s", g_pendingError.title.c_str());

        ImGui::Separator();
        ImGui::Spacing();

        // Message
        ImGui::TextWrapped("%s", g_pendingError.message.c_str());

        ImGui::Spacing();

        // Expandable details
        if (!g_pendingError.details.empty()) {
            const char* expandText = g_pendingError.detailsExpanded ? str.collapseDetails : str.expandDetails;

            if (ImGui::Button(expandText)) {
                g_pendingError.detailsExpanded = !g_pendingError.detailsExpanded;
            }

            if (g_pendingError.detailsExpanded) {
                ImGui::BeginChild("##Details", ImVec2(0, 150), true);
                ImGui::TextUnformatted(g_pendingError.details.c_str());
                ImGui::EndChild();

                ImGui::SameLine();
                if (ImGui::Button(str.copyAll)) {
                    CopyToClipboard(g_pendingError.details);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Close button
        float buttonWidth = 100.0f;
        ImGui::SetCursorPosX((ImGui::GetWindowSize().x - buttonWidth) * 0.5f);
        if (ImGui::Button(str.close, ImVec2(buttonWidth, 0))) {
            g_pendingError.active = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

// Global exception handler
inline LONG WINAPI GlobalExceptionHandler(EXCEPTION_POINTERS* exceptionInfo) {
    ShowFatalCrashDialog(exceptionInfo->ExceptionRecord, exceptionInfo->ContextRecord);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Show early fatal error (before OpenGL/ImGui is available) - uses native Windows dialog
inline void ShowEarlyFatalError(const char* message, const char* details = nullptr) {
    const auto& str = i18n::Get();

    // Try TaskDialogIndirect first for a nicer UI
    typedef HRESULT(WINAPI * TaskDialogIndirectFunc)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE                hComctl             = GetModuleHandleW(L"comctl32.dll");
    TaskDialogIndirectFunc pTaskDialogIndirect = nullptr;
    if (hComctl) {
        pTaskDialogIndirect = (TaskDialogIndirectFunc)GetProcAddress(hComctl, "TaskDialogIndirect");
    }

    if (pTaskDialogIndirect && details) {
        // Convert to wide strings
        int titleLen   = MultiByteToWideChar(CP_UTF8, 0, str.errorTitle, -1, nullptr, 0);
        int msgLen     = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0);
        int detailsLen = MultiByteToWideChar(CP_UTF8, 0, details, -1, nullptr, 0);
        int closeLen   = MultiByteToWideChar(CP_UTF8, 0, str.closeProgram, -1, nullptr, 0);

        std::wstring wTitle(titleLen, 0);
        std::wstring wMsg(msgLen, 0);
        std::wstring wDetails(detailsLen, 0);
        std::wstring wClose(closeLen, 0);

        MultiByteToWideChar(CP_UTF8, 0, str.errorTitle, -1, wTitle.data(), titleLen);
        MultiByteToWideChar(CP_UTF8, 0, message, -1, wMsg.data(), msgLen);
        MultiByteToWideChar(CP_UTF8, 0, details, -1, wDetails.data(), detailsLen);
        MultiByteToWideChar(CP_UTF8, 0, str.closeProgram, -1, wClose.data(), closeLen);

        TASKDIALOGCONFIG config       = {sizeof(config)};
        config.dwFlags                = TDF_EXPAND_FOOTER_AREA | TDF_ALLOW_DIALOG_CANCELLATION;
        config.dwCommonButtons        = 0;
        config.pszWindowTitle         = L"Particle Saturn";
        config.pszMainIcon            = TD_ERROR_ICON;
        config.pszMainInstruction     = wTitle.c_str();
        config.pszContent             = wMsg.c_str();
        config.pszExpandedInformation = wDetails.c_str();

        TASKDIALOG_BUTTON buttons[] = {{IDCLOSE, wClose.c_str()}};
        config.pButtons             = buttons;
        config.cButtons             = 1;
        config.nDefaultButton       = IDCLOSE;

        pTaskDialogIndirect(&config, nullptr, nullptr, nullptr);
    } else {
        // Fallback to MessageBox
        std::string fullMsg = message;
        if (details) {
            fullMsg += "\n\n";
            fullMsg += details;
        }
        MessageBoxA(nullptr, fullMsg.c_str(), "Particle Saturn", MB_OK | MB_ICONERROR);
    }
}

// Initialize error handler
inline void Init() {
    g_startTime = std::chrono::steady_clock::now();

    // Set up global exception handler
    SetUnhandledExceptionFilter(GlobalExceptionHandler);

    // Try to enable visual styles for TaskDialog (may fail on older systems)
    // Load dynamically to avoid ordinal import errors
    HMODULE hComctl = LoadLibraryW(L"comctl32.dll");
    if (hComctl) {
        typedef BOOL(WINAPI * InitCommonControlsExFunc)(const INITCOMMONCONTROLSEX*);
        InitCommonControlsExFunc pInitCommonControlsEx =
            (InitCommonControlsExFunc)GetProcAddress(hComctl, "InitCommonControlsEx");
        if (pInitCommonControlsEx) {
            INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
            pInitCommonControlsEx(&icc);
        }
        // Don't FreeLibrary - keep it loaded for TaskDialog
    }
}

// Update state (call each frame)
inline void UpdateState(int frameCount, int particleCount, float lod, bool handTrackingActive) {
    g_frameCount         = frameCount;
    g_particleCount      = particleCount;
    g_currentLOD         = lod;
    g_handTrackingActive = handTrackingActive;
}

// Set camera info
inline void SetCameraInfo(int index, int width, int height, bool active, const std::string& device = "") {
    g_cameraIndex  = index;
    g_cameraWidth  = width;
    g_cameraHeight = height;
    g_cameraActive = active;
    g_cameraDevice = device;
}

// Set current stage
inline void SetStage(AppStage stage) {
    g_currentStage = stage;
}

} // namespace ErrorHandler
