#pragma once

#include "services/camera/CameraCapture.h"

#include <string>
#include <vector>

struct TfLiteDelegate;
struct TfLiteInterpreter;
struct TfLiteModel;

namespace ParticleSaturn::Services::HandTracking::MacOS {

class XnnpackModel {
public:
    XnnpackModel() = default;
    ~XnnpackModel();
    XnnpackModel(const XnnpackModel&) = delete;
    XnnpackModel& operator=(const XnnpackModel&) = delete;

    bool Load(const std::string& modelPath, std::string& error);
    bool Invoke(const Camera::Frame& frame, std::string& error);
    bool IsLoaded() const noexcept;
    const std::vector<std::vector<float>>& Outputs() const noexcept;

private:
    void Reset() noexcept;

    TfLiteModel* model_ = nullptr;
    TfLiteDelegate* delegate_ = nullptr;
    TfLiteInterpreter* interpreter_ = nullptr;
    int inputWidth_ = 0;
    int inputHeight_ = 0;
    std::vector<std::vector<float>> outputs_;
};

class XnnpackHandTrackingRuntime {
public:
    bool Load(const std::string& palmModelPath, const std::string& landmarkModelPath, std::string& error);
    bool Invoke(const Camera::Frame& frame, std::string& error);
    bool IsLoaded() const noexcept;
    const std::vector<std::vector<float>>& PalmOutputs() const noexcept;
    const std::vector<std::vector<float>>& LandmarkOutputs() const noexcept;

private:
    XnnpackModel palm_;
    XnnpackModel landmark_;
};

} // namespace ParticleSaturn::Services::HandTracking::MacOS
