#pragma once

#include "AVFoundationCamera.h"

#include <string>
#include <vector>

namespace ParticleSaturn::Services::Camera::MacOS {

class CameraSelectorWindow {
public:
    explicit CameraSelectorWindow(AVFoundationCamera& camera);
    ~CameraSelectorWindow();

    CameraSelectorWindow(const CameraSelectorWindow&) = delete;
    CameraSelectorWindow& operator=(const CameraSelectorWindow&) = delete;

    void Show();
    void Refresh();
    void StartSelected();
    std::string SelectedDeviceId() const;

private:
    AVFoundationCamera& camera_;
    std::vector<Device> devices_;
    std::string selectedDeviceId_;
    void* panel_ = nullptr;
    void* popup_ = nullptr;
    void* previewLayer_ = nullptr;
    void* statusLabel_ = nullptr;
    void* delegate_ = nullptr;
};

} // namespace ParticleSaturn::Services::Camera::MacOS
