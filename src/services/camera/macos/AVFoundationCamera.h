#pragma once

#include "services/camera/CameraCapture.h"

#include <mutex>

namespace ParticleSaturn::Services::Camera::MacOS {

class AVFoundationCamera final : public ICameraCapture {
public:
    AVFoundationCamera();
    ~AVFoundationCamera() override;
    Authorization Permission() const override;
    void RequestPermission() override;
    std::vector<Device> Devices() const override;
    bool Start(const std::string& deviceId, std::uint32_t width, std::uint32_t height) override;
    void Stop() override;
    bool IsRunning() const override;
    bool LatestFrame(Frame& frame) override;
    std::string LastError() const override;
    void PublishPixelBuffer(void* pixelBuffer, std::uint64_t timestampNanoseconds);

private:
    mutable std::mutex mutex_;
    void* session_ = nullptr;
    void* output_ = nullptr;
    void* delegate_ = nullptr;
    Frame latestFrame_;
    bool hasFrame_ = false;
    std::string error_;
};

} // namespace ParticleSaturn::Services::Camera::MacOS
