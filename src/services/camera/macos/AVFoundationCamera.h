#pragma once

#include "services/camera/CameraCapture.h"

#include <mutex>

namespace ParticleSaturn::Services::Camera::MacOS {

class AVFoundationCamera final {
public:
    AVFoundationCamera();
    ~AVFoundationCamera();
    Authorization Permission() const;
    void RequestPermission();
    std::vector<Device> Devices() const;
    bool Start(const std::string& deviceId, std::uint32_t width, std::uint32_t height);
    void Stop();
    bool IsRunning() const;
    bool LatestFrame(Frame& frame);
    std::string LastError() const;
    void* NativeSession() const;
    void PublishPixelBuffer(void* pixelBuffer, std::uint64_t timestampNanoseconds);
    void HandleDeviceDisconnected(const char* deviceId);

private:
    mutable std::mutex mutex_;
    void* session_ = nullptr;
    void* output_ = nullptr;
    void* delegate_ = nullptr;
    void* disconnectObserver_ = nullptr;
    std::string activeDeviceId_;
    Frame latestFrame_;
    bool hasFrame_ = false;
    std::string error_;
};

} // namespace ParticleSaturn::Services::Camera::MacOS
