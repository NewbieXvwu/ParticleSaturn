#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ParticleSaturn::Services::Camera {

enum class Authorization { NotDetermined, Authorized, Denied, Restricted };
enum class PixelFormat { RGB24, BGRA32 };
enum class FrameOrientation { Up, Down, Left, Right };

struct Device {
    std::string id;
    std::string name;
    bool connected = false;
};

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::uint32_t bytesPerRow = 0;
    PixelFormat pixelFormat = PixelFormat::RGB24;
    FrameOrientation orientation = FrameOrientation::Up;
    bool mirrored = false;
    std::vector<std::uint8_t> pixels;
};

class ICameraCapture {
public:
    virtual ~ICameraCapture() = default;
    virtual Authorization Permission() const = 0;
    virtual void RequestPermission() = 0;
    virtual std::vector<Device> Devices() const = 0;
    virtual bool Start(const std::string& deviceId, std::uint32_t width, std::uint32_t height) = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual bool LatestFrame(Frame& frame) = 0;
    virtual std::string LastError() const = 0;
};

} // namespace ParticleSaturn::Services::Camera
