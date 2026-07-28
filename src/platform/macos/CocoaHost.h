#pragma once

#include "app/RenderSeam.h"
#include "app/state/AppStates.h"

#include <cstdint>
#include <functional>

namespace ParticleSaturn::Platform::MacOS {

// 中立化后（D-015 重启）：可绘制表面尺寸的规范定义移入 app/RenderSeam.h，
// 此处保留 DrawableSize 名字作别名，宿主/全屏/Retina 语义不变。
using DrawableSize = App::SurfaceSize;

enum class HostAction : std::uint8_t {
    ToggleDebugWindow,
    ToggleFullscreen,
    ToggleBlur,
    TogglePause,
    ShowCameraSelector,
    KeyF3Down,
    KeyF3Up,
    KeyF11Down,
    KeyF11Up,
    KeyBDown,
    KeyBUp,
    KeyEscapeDown,
    KeyEscapeUp,
    NativeFullscreenEntered,
    NativeFullscreenExited,
};

class CocoaHost {
public:
    CocoaHost(std::uint32_t width, std::uint32_t height, const char* title);
    ~CocoaHost();

    CocoaHost(const CocoaHost&) = delete;
    CocoaHost& operator=(const CocoaHost&) = delete;

    void Show();
    void Run(const std::function<void()>& frameCallback = {});
    void SetActionCallback(std::function<void(HostAction)> callback);
    void InvokeAction(HostAction action);
    void SetWindowPosition(std::int32_t x, std::int32_t y);
    void GetWindowPosition(std::int32_t& x, std::int32_t& y) const;
    void ToggleFullscreen();
    void SetFullscreenActive(bool active);
    void RequestExit();
    // 结束 [NSApp run] 并让控制流回到调用方 main，使失败退出码可传播
    // （terminate: 恒以 0 结束进程，main 里的 return 1 永远执行不到）。
    // 供未持有 CocoaHost 实例的宿主（OpenGL41Main）复用同一退出语义。
    static void StopRunLoop();
    void SetPresentationMode(int vsyncMode);
    void SetWindowMaterial(App::WindowMaterial material);
    DrawableSize CurrentDrawableSize() const;
    void* NativeMetalLayer() const noexcept;
    void* NativeView() const noexcept;

private:
    void PresentFullscreenBackdrop();

    void* window_ = nullptr;
    void* layer_ = nullptr;
    void* metalView_ = nullptr;
    void* visualEffectView_ = nullptr;
    void* windowDelegate_ = nullptr;
    void* eventMonitor_ = nullptr;
    std::function<void(HostAction)> actionCallback_;
    App::WindowMaterial windowMaterial_ = App::WindowMaterial::Solid;
    bool fullscreen_ = false;
};

} // namespace ParticleSaturn::Platform::MacOS
