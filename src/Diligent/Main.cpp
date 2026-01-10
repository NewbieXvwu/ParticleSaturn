#include "DiligentBackend.h"
#include "RenderBackend.h"

#include <windows.h>

#include <string>

namespace {

constexpr wchar_t kWindowClassName[] = L"ParticleSaturn.Diligent";
constexpr wchar_t kWindowTitle[]     = L"Particle Saturn (Diligent)";

ParticleSaturn::Render::Backend ParseBackendFromCmdLine(const std::wstring& cmdLine) {
    // 支持：
    //   --backend=d3d12
    //   --backend=vulkan
    // 默认：D3D12（Windows 上更常见，且无需 Vulkan SDK 即可运行）。
    if (cmdLine.find(L"--backend=vulkan") != std::wstring::npos || cmdLine.find(L"--backend=vk") != std::wstring::npos) {
        return ParticleSaturn::Render::Backend::Vulkan;
    }
    return ParticleSaturn::Render::Backend::D3D12;
}

ParticleSaturn::Render::SurfaceSize GetClientSize(HWND hwnd) {
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return {};
    }
    const auto w = static_cast<uint32_t>(rc.right - rc.left);
    const auto h = static_cast<uint32_t>(rc.bottom - rc.top);
    return {w, h};
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* backend = reinterpret_cast<ParticleSaturn::Render::DiligentBackend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_SIZE: {
            if (backend != nullptr && wParam != SIZE_MINIMIZED) {
                const auto w = static_cast<uint32_t>(LOWORD(lParam));
                const auto h = static_cast<uint32_t>(HIWORD(lParam));
                backend->Resize({w, h});
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    const std::wstring cmdLine = GetCommandLineW();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);

    const DWORD style   = WS_OVERLAPPEDWINDOW;
    const DWORD exStyle = 0;

    RECT wr{0, 0, 1280, 720};
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);

    HWND hwnd = CreateWindowExW(exStyle, kWindowClassName, kWindowTitle, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, hInstance, nullptr);
    if (hwnd == nullptr) {
        return -1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    ParticleSaturn::Render::DiligentBackend backend{};
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&backend));

    const auto surface = GetClientSize(hwnd);
    if (!backend.Init(ParseBackendFromCmdLine(cmdLine), hwnd, surface)) {
        MessageBoxW(hwnd, L"初始化 Diligent 失败。请确认 D3D12/Vulkan 环境可用。", kWindowTitle, MB_ICONERROR | MB_OK);
        return -2;
    }

    MSG msg{};
    while (true) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                backend.Shutdown();
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        backend.RenderFrame();
    }
}
