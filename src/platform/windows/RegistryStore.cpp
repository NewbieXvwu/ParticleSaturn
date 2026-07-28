// RegistryStore - Windows 注册表持久化实现（D-015 重启 · 决策②：服务落 src/platform/windows/）
//
// 本文件从 src/Diligent/Settings.cpp 平移而来（API/命名空间不变），把设置持久化这一
// "服务"职责从渲染后端目录挪进平台树，镜像 macOS 的 services/settings/macos/。
// 低层原语（Reg* / CreateProcessW 重启）为 Windows 专有，Phase C 收敛为
// Services::Settings::Windows::RegistryStore 类时仍原样复用，仅高层 Save/Load 签名
// 改吃 App::AppState。

#include "../../Settings.h"

#include <iostream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../AppState.h"
#include "RenderBackend.h"
#include "imgui.h"

namespace Settings {

// ============================================================================
// 基础注册表操作
// ============================================================================

void SetDWORD(const wchar_t* name, uint32_t value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey,
                        nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(hKey);
    }
}

uint32_t GetDWORD(const wchar_t* name, uint32_t defaultValue) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD value = 0;
        DWORD size  = sizeof(value);
        DWORD type  = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            RegCloseKey(hKey);
            return value;
        }
        RegCloseKey(hKey);
    }
    return defaultValue;
}

void SetString(const wchar_t* name, const std::string& value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey,
                        nullptr) == ERROR_SUCCESS) {
        // 存储为 REG_SZ（需要转换为宽字符）或 REG_BINARY
        // 这里用 REG_BINARY 更简单，直接存 UTF-8 字节
        RegSetValueExW(hKey, name, 0, REG_BINARY, reinterpret_cast<const BYTE*>(value.data()),
                       static_cast<DWORD>(value.size()));
        RegCloseKey(hKey);
    }
}

std::string GetString(const wchar_t* name) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        // 先查询大小
        DWORD size = 0;
        DWORD type = 0;
        if (RegQueryValueExW(hKey, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_BINARY &&
            size > 0) {
            std::string result(size, '\0');
            if (RegQueryValueExW(hKey, name, nullptr, nullptr, reinterpret_cast<BYTE*>(result.data()), &size) ==
                ERROR_SUCCESS) {
                RegCloseKey(hKey);
                return result;
            }
        }
        RegCloseKey(hKey);
    }
    return {};
}

// ============================================================================
// 高级 API
// ============================================================================

void SaveSession(const AppState& state, ParticleSaturn::Render::Backend backend) {
    std::cout << "[Settings] Saving session to registry..." << std::endl;

    // Backend
    SaveBackend(backend);

    // 窗口状态
    SaveWindowState(state.window.windowedX, state.window.windowedY, state.window.windowedW, state.window.windowedH,
                    state.window.isFullscreen);

    // UI 状态
    SetDWORD(L"UI_ShowDebugWindow", state.ui.showDebugWindow ? 1 : 0);
    SetDWORD(L"UI_EnableBlur", state.ui.enableBlur ? 1 : 0);
    SetDWORD(L"UI_BlurStrength", static_cast<uint32_t>(state.ui.blurStrength * 100.0f));
    SetDWORD(L"UI_NoiseIntensity", static_cast<uint32_t>(state.ui.noiseIntensity * 10000.0f));

    // 渲染状态
    SetDWORD(L"Render_PixelRatio", static_cast<uint32_t>(state.render.pixelRatio * 100.0f));
    SetDWORD(L"Render_ParticleCount", state.render.activeParticleCount);
    // vsyncMode: -1, 0, 1 → 存储为 0, 1, 2（避免负数）
    SetDWORD(L"Render_VSync", static_cast<uint32_t>(state.render.vsyncMode + 1));

    // Backdrop 状态
    SetDWORD(L"Backdrop_Index", static_cast<uint32_t>(state.backdrop.backdropIndex));
    SetDWORD(L"Backdrop_Transparent", state.backdrop.useTransparent ? 1 : 0);

    // LOD 状态
    SetDWORD(L"LOD_Locked", state.lod.locked ? 1 : 0);

    // ImGui 布局
    SaveImGuiLayout();

    std::cout << "[Settings] Session saved" << std::endl;
}

bool LoadSession(AppState& state) {
    std::cout << "[Settings] Loading session from registry..." << std::endl;

    // 检查是否有保存的设置（用 Backend 作为标志）
    int savedBackend = GetSavedBackend();
    if (savedBackend < 0) {
        std::cout << "[Settings] No saved session found" << std::endl;
        return false;
    }

    // 窗口状态
    WindowState ws = LoadWindowState();
    if (ws.valid) {
        state.window.windowedX    = ws.x;
        state.window.windowedY    = ws.y;
        state.window.windowedW    = ws.w;
        state.window.windowedH    = ws.h;
        state.window.isFullscreen = ws.fullscreen;
    }

    // UI 状态
    state.ui.showDebugWindow = GetDWORD(L"UI_ShowDebugWindow", 0) != 0;
    state.ui.enableBlur      = GetDWORD(L"UI_EnableBlur", 1) != 0;
    state.ui.blurStrength    = static_cast<float>(GetDWORD(L"UI_BlurStrength", 200)) / 100.0f;
    state.ui.noiseIntensity  = static_cast<float>(GetDWORD(L"UI_NoiseIntensity", 100)) / 10000.0f;

    // 渲染状态
    state.render.pixelRatio          = static_cast<float>(GetDWORD(L"Render_PixelRatio", 100)) / 100.0f;
    state.render.activeParticleCount = GetDWORD(L"Render_ParticleCount", 1200000);
    // vsyncMode: 存储值 0, 1, 2 → 实际值 -1, 0, 1
    state.render.vsyncMode = static_cast<int>(GetDWORD(L"Render_VSync", 2)) - 1;

    // Backdrop 状态
    state.backdrop.backdropIndex  = static_cast<int>(GetDWORD(L"Backdrop_Index", 0));
    state.backdrop.useTransparent = GetDWORD(L"Backdrop_Transparent", 0) != 0;

    // LOD 状态
    state.lod.locked = GetDWORD(L"LOD_Locked", 0) != 0;

    std::cout << "[Settings] Session loaded" << std::endl;
    return true;
}

int GetSavedBackend() {
    uint32_t value = GetDWORD(L"Backend", UINT32_MAX);
    if (value == UINT32_MAX) {
        return -1;
    }
    return static_cast<int>(value);
}

void SaveBackend(ParticleSaturn::Render::Backend backend) {
    SetDWORD(L"Backend", static_cast<uint32_t>(backend));
}

// ============================================================================
// ImGui 布局
// ============================================================================

void SaveImGuiLayout() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    size_t      size    = 0;
    const char* iniData = ImGui::SaveIniSettingsToMemory(&size);
    if (iniData != nullptr && size > 0) {
        SetString(L"ImGui_Layout", std::string(iniData, size));
        std::cout << "[Settings] ImGui layout saved (" << size << " bytes)" << std::endl;
    }
}

void LoadImGuiLayout() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    std::string iniData = GetString(L"ImGui_Layout");
    if (!iniData.empty()) {
        ImGui::LoadIniSettingsFromMemory(iniData.c_str(), iniData.size());
        std::cout << "[Settings] ImGui layout loaded (" << iniData.size() << " bytes)" << std::endl;
    }
}

// ============================================================================
// 窗口状态
// ============================================================================

void SaveWindowState(int x, int y, int w, int h, bool fullscreen) {
    SetDWORD(L"Window_X", static_cast<uint32_t>(x));
    SetDWORD(L"Window_Y", static_cast<uint32_t>(y));
    SetDWORD(L"Window_W", static_cast<uint32_t>(w));
    SetDWORD(L"Window_H", static_cast<uint32_t>(h));
    SetDWORD(L"Window_Fullscreen", fullscreen ? 1 : 0);
}

WindowState LoadWindowState() {
    WindowState ws;

    // 检查是否有保存的窗口位置（用 Window_W 作为标志）
    uint32_t savedW = GetDWORD(L"Window_W", 0);
    if (savedW == 0) {
        ws.valid = false;
        return ws;
    }

    ws.x          = static_cast<int>(GetDWORD(L"Window_X", 100));
    ws.y          = static_cast<int>(GetDWORD(L"Window_Y", 100));
    ws.w          = static_cast<int>(savedW);
    ws.h          = static_cast<int>(GetDWORD(L"Window_H", 720));
    ws.fullscreen = GetDWORD(L"Window_Fullscreen", 0) != 0;
    ws.valid      = true;

    return ws;
}

// ============================================================================
// 进程重启
// ============================================================================

bool RestartWithBackend(ParticleSaturn::Render::Backend newBackend, const AppState& state) {
    std::cout << "[Settings] Restarting with new backend..." << std::endl;

    // 1. 保存当前会话（包括新的 Backend）
    SaveSession(state, newBackend);

    // 2. 获取当前可执行文件路径
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        std::cerr << "[Settings] Failed to get executable path" << std::endl;
        return false;
    }

    // 3. 构建命令行（双保险：传递 --backend 参数）
    const wchar_t* backendArg = nullptr;
    switch (newBackend) {
    case ParticleSaturn::Render::Backend::D3D11:
        backendArg = L"--backend=d3d11";
        break;
    case ParticleSaturn::Render::Backend::D3D12:
        backendArg = L"--backend=d3d12";
        break;
    case ParticleSaturn::Render::Backend::Vulkan:
        backendArg = L"--backend=vulkan";
        break;
    default:
        backendArg = L"";
        break;
    }

    std::wstring cmdLine = L"\"";
    cmdLine += exePath;
    cmdLine += L"\" ";
    cmdLine += backendArg;

    std::wcout << L"[Settings] Starting: " << cmdLine << std::endl;

    // 4. 启动新进程
    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};

    // 注意：CreateProcessW 需要可写的命令行缓冲区
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr,           // 使用命令行中的路径
                        cmdLineBuf.data(), // 命令行
                        nullptr,           // 进程安全属性
                        nullptr,           // 线程安全属性
                        FALSE,             // 不继承句柄
                        0,                 // 创建标志
                        nullptr,           // 使用父进程环境
                        nullptr,           // 使用父进程工作目录
                        &si, &pi)) {
        std::cerr << "[Settings] CreateProcess failed: " << GetLastError() << std::endl;
        return false;
    }

    // 关闭句柄（我们不需要等待新进程）
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::cout << "[Settings] New process started, exiting current process..." << std::endl;
    return true;
}

} // namespace Settings
