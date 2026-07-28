#include "Win32AppHost.h"

#include <iostream>
#include <shellscalingapi.h> // GetDpiForWindow (Win10 1607+)

#include "../../Settings.h"

namespace ParticleSaturn::Platform::Windows {

bool IsDirectCompositionSupported() {
    // 使用 RtlGetVersion 获取真实版本（不受兼容性 shim 影响）
    using RtlGetVersionPtr = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }

    auto RtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (RtlGetVersion == nullptr) {
        return false;
    }

    RTL_OSVERSIONINFOW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    if (RtlGetVersion(&osvi) != 0) {
        return false;
    }

    // Win10 1803+ (Build 17134+) 支持 WS_EX_NOREDIRECTIONBITMAP + DirectComposition
    // Win11 (Build 22000+) 支持 Mica
    // 保守起见，只在 Win10 1809+ (Build 17763+) 启用
    const bool isWin10_1809OrLater =
        (osvi.dwMajorVersion > 10) || (osvi.dwMajorVersion == 10 && osvi.dwBuildNumber >= 17763);

    if (isWin10_1809OrLater) {
        std::cout << "[Main] Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << " Build "
                  << osvi.dwBuildNumber << " - DirectComposition supported" << std::endl;
        return true;
    }

    std::cout << "[Main] Windows " << osvi.dwMajorVersion << "." << osvi.dwMinorVersion << " Build "
              << osvi.dwBuildNumber << " - DirectComposition NOT supported" << std::endl;
    return false;
}

float GetDpiScaleForWindow(HWND hwnd) {
    // 优先使用 GetDpiForWindow (Win10 1607+)
    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) {
        // 回退到系统 DPI
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            dpi = static_cast<UINT>(GetDeviceCaps(hdc, LOGPIXELSX));
            ReleaseDC(hwnd, hdc);
        }
    }
    if (dpi == 0) {
        dpi = 96; // 默认 96 DPI
    }
    return static_cast<float>(dpi) / 96.0f;
}

Render::Backend ParseBackendFromCmdLine(const std::wstring& cmdLine) {
    // 优先级：命令行参数 > 注册表 > 默认值
    //
    // 支持：
    //   --backend=d3d11
    //   --backend=d3d12
    //   --backend=vulkan

    // 1. 命令行参数优先
    if (cmdLine.find(L"--backend=vulkan") != std::wstring::npos ||
        cmdLine.find(L"--backend=vk") != std::wstring::npos) {
        return Render::Backend::Vulkan;
    }
    if (cmdLine.find(L"--backend=d3d11") != std::wstring::npos ||
        cmdLine.find(L"--backend=dx11") != std::wstring::npos) {
        return Render::Backend::D3D11;
    }
    if (cmdLine.find(L"--backend=d3d12") != std::wstring::npos ||
        cmdLine.find(L"--backend=dx12") != std::wstring::npos) {
        return Render::Backend::D3D12;
    }

    // 2. 注册表次之
    int savedBackend = Settings::GetSavedBackend();
    if (savedBackend >= 0 && savedBackend <= 2) {
        std::cout << "[Main] Using saved backend from registry" << std::endl;
        return static_cast<Render::Backend>(savedBackend);
    }

    // 3. 默认 D3D12
    return Render::Backend::D3D12;
}

Render::SurfaceSize GetClientSize(HWND hwnd) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return {};
    }
    const auto w = static_cast<uint32_t>(rc.right - rc.left);
    const auto h = static_cast<uint32_t>(rc.bottom - rc.top);
    return {w, h};
}

} // namespace ParticleSaturn::Platform::Windows
