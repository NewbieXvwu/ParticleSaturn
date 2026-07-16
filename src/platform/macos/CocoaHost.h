#pragma once

#include <cstdint>

namespace ParticleSaturn::Platform::MacOS {

struct DrawableSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    float scale = 1.0f;
};

class CocoaHost {
public:
    CocoaHost(std::uint32_t width, std::uint32_t height, const char* title);
    ~CocoaHost();

    CocoaHost(const CocoaHost&) = delete;
    CocoaHost& operator=(const CocoaHost&) = delete;

    void Show();
    void Run();
    void ToggleFullscreen();
    DrawableSize CurrentDrawableSize() const;
    void* NativeMetalLayer() const noexcept;

private:
    void* window_ = nullptr;
    void* layer_ = nullptr;
};

} // namespace ParticleSaturn::Platform::MacOS
