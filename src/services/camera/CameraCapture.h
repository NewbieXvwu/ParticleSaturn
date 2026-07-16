#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ParticleSaturn::Services::Camera {

enum class Authorization { NotDetermined, Authorized, Denied, Restricted };

struct Device {
    std::string id;
    std::string name;
    bool connected = false;
};

struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t timestampNanoseconds = 0;
    std::vector<std::uint8_t> rgb;
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
