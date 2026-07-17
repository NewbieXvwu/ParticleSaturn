#pragma once

#include "services/camera/CameraCapture.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct TfLiteDelegate;
struct TfLiteInterpreter;
struct TfLiteModel;

namespace ParticleSaturn::Services::HandTracking::MacOS {

struct PalmRegion {
    float confidence = 0.0f;
    float centerX = 0.5f;
    float centerY = 0.5f;
    float width = 0.0f;
    float height = 0.0f;
    float rotation = 0.0f;
    std::array<float, 14> keypoints{};
    float handCenterX = 0.5f;
    float handCenterY = 0.5f;
    float handSide = 0.0f;
    bool isLeftHand = false;
};

struct HandPose {
    float confidence = 0.0f;
    float centerX = 0.5f;
    float centerY = 0.5f;
    float scale = 1.0f;
};

enum class PreprocessingPath { ScalarFused, NeonFused };

struct PreprocessingStats {
    PreprocessingPath path = PreprocessingPath::ScalarFused;
    std::uint64_t pixels = 0;
    std::uint64_t elapsedNanoseconds = 0;
};

// 以一次采样、颜色转换和归一化写入 NHWC float32 张量，不创建中间 RGB 或浮点图像。
bool PreprocessCameraFrameToTensor(const Camera::Frame& frame, std::uint32_t targetWidth,
                                   std::uint32_t targetHeight, float* target, std::string& error,
                                   PreprocessingStats* stats = nullptr);

class XnnpackModel {
public:
    XnnpackModel() = default;
    ~XnnpackModel();
    XnnpackModel(const XnnpackModel&) = delete;
    XnnpackModel& operator=(const XnnpackModel&) = delete;

    bool Load(const std::string& modelPath, std::string& error);
    bool Invoke(const Camera::Frame& frame, std::string& error);
    void ClearOutputs() noexcept;
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
    bool DecodePalm(PalmRegion& region) const;
    bool DecodeLandmarks(HandPose& pose) const;
    static void ExpandPalmToHandRegion(PalmRegion& region) noexcept;
    static bool DecodeLandmarkOutputs(const std::vector<std::vector<float>>& outputs, const PalmRegion& region,
                                      HandPose& pose) noexcept;

private:
    XnnpackModel palm_;
    XnnpackModel landmark_;
    PalmRegion lastRegion_;
    bool hasRegion_ = false;
};

} // namespace ParticleSaturn::Services::HandTracking::MacOS
