#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ParticleSaturn::Services::Camera {

enum class Authorization {
    NotDetermined,
    Authorized,
    Denied,
    Restricted
};
enum class PixelFormat {
    RGB24,
    BGRA32
};
enum class FrameOrientation {
    Up,
    Down,
    Left,
    Right
};

struct Device {
    std::string id;
    std::string name;
    bool        connected = false;
};

struct Frame {
    std::uint32_t             width                = 0;
    std::uint32_t             height               = 0;
    std::uint64_t             timestampNanoseconds = 0;
    std::uint32_t             bytesPerRow          = 0;
    PixelFormat               pixelFormat          = PixelFormat::RGB24;
    FrameOrientation          orientation          = FrameOrientation::Up;
    bool                      mirrored             = false;
    std::vector<std::uint8_t> pixels;
};

} // namespace ParticleSaturn::Services::Camera
