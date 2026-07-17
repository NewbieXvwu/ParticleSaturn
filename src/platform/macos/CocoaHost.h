#pragma once

#include "app/state/AppStates.h"

#include <cstdint>
#include <functional>

namespace ParticleSaturn::Platform::MacOS {

struct DrawableSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float scale = 1.0f;
};

enum class HostAction : std::uint8_t {
    ToggleDebugWindow,
    ToggleFullscreen,
    ToggleBlur,
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
    void SetWindowPosition(std::int32_t x, std::int32_t y);
    void GetWindowPosition(std::int32_t& x, std::int32_t& y) const;
    void ToggleFullscreen();
    void SetFullscreenActive(bool active);
    void RequestExit();
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
