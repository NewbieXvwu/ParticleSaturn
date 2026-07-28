#include "XnnpackRuntime.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "services/diagnostics/DiagnosticBus.h"
#include "tensorflow/lite/core/c/c_api.h"
#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace ParticleSaturn::Services::HandTracking::MacOS {

namespace {
bool ReportFailure(std::string& error, const char* code) {
    ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Publish(
        "hand-tracking", code, error, ParticleSaturn::Services::Diagnostics::Severity::Error);
    return false;
}
} // namespace

namespace {

bool FrameLayoutIsValid(const Camera::Frame& frame) {
    if (frame.width == 0 || frame.height == 0) {
        return false;
    }
    const std::uint32_t bytesPerPixel = frame.pixelFormat == Camera::PixelFormat::BGRA32 ? 4U : 3U;
    if (frame.bytesPerRow < frame.width * bytesPerPixel) {
        return false;
    }
    return frame.pixels.size() >= static_cast<std::size_t>(frame.bytesPerRow) * frame.height;
}

void MapOrientedPixel(const Camera::Frame& frame, std::uint32_t orientedX, std::uint32_t orientedY,
                      std::uint32_t& sourceX, std::uint32_t& sourceY) {
    switch (frame.orientation) {
    case Camera::FrameOrientation::Up:
        sourceX = orientedX;
        sourceY = orientedY;
        break;
    case Camera::FrameOrientation::Down:
        sourceX = frame.width - 1U - orientedX;
        sourceY = frame.height - 1U - orientedY;
        break;
    case Camera::FrameOrientation::Left:
        sourceX = frame.width - 1U - orientedY;
        sourceY = orientedX;
        break;
    case Camera::FrameOrientation::Right:
        sourceX = orientedY;
        sourceY = frame.height - 1U - orientedX;
        break;
    }
}

void RecordPreprocessingStats(PreprocessingStats* stats, PreprocessingPath path, std::uint32_t width,
                              std::uint32_t height, std::chrono::steady_clock::time_point startedAt) {
    if (stats == nullptr) {
        return;
    }
    stats->path               = path;
    stats->pixels             = static_cast<std::uint64_t>(width) * height;
    stats->elapsedNanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - startedAt).count());
}

#if defined(__aarch64__) || defined(__ARM_NEON)
float32x4_t Normalize4(uint8x8_t values, bool high) {
    const uint16x8_t widened16  = vmovl_u8(values);
    const uint16x4_t selected16 = high ? vget_high_u16(widened16) : vget_low_u16(widened16);
    return vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(selected16)), 1.0f / 255.0f);
}

void StoreNormalizedRgb8(const uint8x8_t red, const uint8x8_t green, const uint8x8_t blue, float* target) {
    const float32x4x3_t low{{Normalize4(red, false), Normalize4(green, false), Normalize4(blue, false)}};
    const float32x4x3_t high{{Normalize4(red, true), Normalize4(green, true), Normalize4(blue, true)}};
    vst3q_f32(target, low);
    vst3q_f32(target + 12, high);
}
#endif

} // namespace

bool PreprocessCameraFrameToTensor(const Camera::Frame& frame, std::uint32_t targetWidth, std::uint32_t targetHeight,
                                   float* target, std::string& error, PreprocessingStats* stats) {
    const auto startedAt = std::chrono::steady_clock::now();
    if (!FrameLayoutIsValid(frame) || targetWidth == 0 || targetHeight == 0 || target == nullptr) {
        error = "camera frame layout is invalid";
        return ReportFailure(error, "frame-layout");
    }
    const bool rotated =
        frame.orientation == Camera::FrameOrientation::Left || frame.orientation == Camera::FrameOrientation::Right;
    const std::uint32_t orientedWidth  = rotated ? frame.height : frame.width;
    const std::uint32_t orientedHeight = rotated ? frame.width : frame.height;
    const std::uint32_t bytesPerPixel  = frame.pixelFormat == Camera::PixelFormat::BGRA32 ? 4U : 3U;
    constexpr float     scale          = 1.0f / 255.0f;
#if defined(__aarch64__) || defined(__ARM_NEON)
    if (!rotated && !frame.mirrored && targetWidth == frame.width && targetHeight == frame.height) {
        for (std::uint32_t y = 0; y < targetHeight; ++y) {
            const auto*   source      = frame.pixels.data() + static_cast<std::size_t>(y) * frame.bytesPerRow;
            auto*         destination = target + static_cast<std::size_t>(y) * targetWidth * 3U;
            std::uint32_t x           = 0;
            if (frame.pixelFormat == Camera::PixelFormat::BGRA32) {
                for (; x + 8U <= targetWidth; x += 8U) {
                    const uint8x8x4_t bgra = vld4_u8(source + x * 4U);
                    StoreNormalizedRgb8(bgra.val[2], bgra.val[1], bgra.val[0], destination + x * 3U);
                }
            } else {
                for (; x + 8U <= targetWidth; x += 8U) {
                    const uint8x8x3_t rgb = vld3_u8(source + x * 3U);
                    StoreNormalizedRgb8(rgb.val[0], rgb.val[1], rgb.val[2], destination + x * 3U);
                }
            }
            for (; x < targetWidth; ++x) {
                const auto* pixel  = source + x * bytesPerPixel;
                auto*       output = destination + x * 3U;
                if (frame.pixelFormat == Camera::PixelFormat::BGRA32) {
                    output[0] = pixel[2] * scale;
                    output[1] = pixel[1] * scale;
                    output[2] = pixel[0] * scale;
                } else {
                    output[0] = pixel[0] * scale;
                    output[1] = pixel[1] * scale;
                    output[2] = pixel[2] * scale;
                }
            }
        }
        error.clear();
        RecordPreprocessingStats(stats, PreprocessingPath::NeonFused, targetWidth, targetHeight, startedAt);
        return true;
    }
#endif
    for (std::uint32_t y = 0; y < targetHeight; ++y) {
        const std::uint32_t orientedY = std::min(orientedHeight - 1U, y * orientedHeight / targetHeight);
        for (std::uint32_t x = 0; x < targetWidth; ++x) {
            std::uint32_t orientedX = std::min(orientedWidth - 1U, x * orientedWidth / targetWidth);
            if (frame.mirrored) {
                orientedX = orientedWidth - 1U - orientedX;
            }
            std::uint32_t sourceX = 0;
            std::uint32_t sourceY = 0;
            MapOrientedPixel(frame, orientedX, orientedY, sourceX, sourceY);
            const auto* source = frame.pixels.data() + static_cast<std::size_t>(sourceY) * frame.bytesPerRow +
                                 static_cast<std::size_t>(sourceX) * bytesPerPixel;
            auto* destination = target + (static_cast<std::size_t>(y) * targetWidth + x) * 3U;
            if (frame.pixelFormat == Camera::PixelFormat::BGRA32) {
                destination[0] = source[2] * scale;
                destination[1] = source[1] * scale;
                destination[2] = source[0] * scale;
            } else {
                destination[0] = source[0] * scale;
                destination[1] = source[1] * scale;
                destination[2] = source[2] * scale;
            }
        }
    }
    error.clear();
    RecordPreprocessingStats(stats, PreprocessingPath::ScalarFused, targetWidth, targetHeight, startedAt);
    return true;
}

XnnpackModel::~XnnpackModel() {
    Reset();
}

void XnnpackModel::Reset() noexcept {
    if (interpreter_ != nullptr) {
        TfLiteInterpreterDelete(interpreter_);
    }
    if (delegate_ != nullptr) {
        TfLiteXNNPackDelegateDelete(delegate_);
    }
    if (model_ != nullptr) {
        TfLiteModelDelete(model_);
    }
    interpreter_ = nullptr;
    delegate_    = nullptr;
    model_       = nullptr;
    inputWidth_  = 0;
    inputHeight_ = 0;
    outputs_.clear();
}

bool XnnpackModel::Load(const std::string& modelPath, std::string& error) {
    Reset();
    model_ = TfLiteModelCreateFromFile(modelPath.c_str());
    if (model_ == nullptr) {
        error = "unable to load TensorFlow Lite model: " + modelPath;
        return ReportFailure(error, "model-load");
    }
    TfLiteXNNPackDelegateOptions delegateOptions = TfLiteXNNPackDelegateOptionsDefault();
    delegateOptions.num_threads                  = 0;
    delegate_                                    = TfLiteXNNPackDelegateCreate(&delegateOptions);
    if (delegate_ == nullptr) {
        error = "unable to create XNNPACK delegate";
        Reset();
        return ReportFailure(error, "delegate-create");
    }
    TfLiteInterpreterOptions* options = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(options, 0);
    TfLiteInterpreterOptionsAddDelegate(options, delegate_);
    interpreter_ = TfLiteInterpreterCreate(model_, options);
    TfLiteInterpreterOptionsDelete(options);
    if (interpreter_ == nullptr || TfLiteInterpreterAllocateTensors(interpreter_) != kTfLiteOk) {
        error = "unable to allocate XNNPACK interpreter tensors";
        Reset();
        return ReportFailure(error, "tensor-allocation");
    }
    TfLiteTensor* input = TfLiteInterpreterGetInputTensor(interpreter_, 0);
    if (input == nullptr || TfLiteTensorType(input) != kTfLiteFloat32 || TfLiteTensorNumDims(input) != 4 ||
        TfLiteTensorDim(input, 0) != 1 || TfLiteTensorDim(input, 3) != 3) {
        error = "hand model must expose one NHWC float32 RGB input";
        Reset();
        return ReportFailure(error, "input-contract");
    }
    inputHeight_ = TfLiteTensorDim(input, 1);
    inputWidth_  = TfLiteTensorDim(input, 2);
    if (inputWidth_ <= 0 || inputHeight_ <= 0) {
        error = "hand model input dimensions are invalid";
        Reset();
        return ReportFailure(error, "input-dimensions");
    }
    error.clear();
    return true;
}

bool XnnpackModel::Invoke(const Camera::Frame& frame, std::string& error) {
    if (!IsLoaded()) {
        error = "XNNPACK model is not loaded";
        return false;
    }
    TfLiteTensor* tensor = TfLiteInterpreterGetInputTensor(interpreter_, 0);
    auto*         input  = static_cast<float*>(TfLiteTensorData(tensor));
    if (input == nullptr) {
        error = "XNNPACK input tensor is unavailable";
        return false;
    }
    if (!PreprocessCameraFrameToTensor(frame, static_cast<std::uint32_t>(inputWidth_),
                                       static_cast<std::uint32_t>(inputHeight_), input, error)) {
        return false;
    }
    if (TfLiteInterpreterInvoke(interpreter_) != kTfLiteOk) {
        error = "XNNPACK model invocation failed";
        return false;
    }
    // 输出缓冲跨帧复用：尺寸稳定后不再分配（AUDIT P2-8）。
    const int outputCount = TfLiteInterpreterGetOutputTensorCount(interpreter_);
    outputs_.resize(static_cast<std::size_t>(outputCount));
    for (int index = 0; index < outputCount; ++index) {
        const TfLiteTensor* output = TfLiteInterpreterGetOutputTensor(interpreter_, index);
        if (output == nullptr || TfLiteTensorType(output) != kTfLiteFloat32) {
            error = "hand model must expose float32 outputs";
            outputs_.clear();
            return false;
        }
        std::size_t elementCount = 1;
        for (int dimension = 0; dimension < TfLiteTensorNumDims(output); ++dimension) {
            elementCount *= static_cast<std::size_t>(TfLiteTensorDim(output, dimension));
        }
        auto& values = outputs_[static_cast<std::size_t>(index)];
        values.resize(elementCount);
        if (TfLiteTensorCopyToBuffer(output, values.data(), values.size() * sizeof(float)) != kTfLiteOk) {
            error = "unable to read hand model output";
            outputs_.clear();
            return false;
        }
    }
    error.clear();
    return true;
}

bool XnnpackModel::IsLoaded() const noexcept {
    return interpreter_ != nullptr;
}

const std::vector<std::vector<float>>& XnnpackModel::Outputs() const noexcept {
    return outputs_;
}

void XnnpackModel::ClearOutputs() noexcept {
    outputs_.clear();
}

bool XnnpackHandTrackingRuntime::Load(const std::string& palmModelPath, const std::string& landmarkModelPath,
                                      std::string& error) {
    if (!palm_.Load(palmModelPath, error)) {
        return false;
    }
    if (!landmark_.Load(landmarkModelPath, error)) {
        return false;
    }
    return true;
}

bool XnnpackHandTrackingRuntime::Invoke(const Camera::Frame& frame, std::string& error) {
    if (!palm_.Invoke(frame, error)) {
        return false;
    }
    PalmRegion region;
    if (!DecodePalm(region)) {
        landmark_.ClearOutputs();
        hasRegion_ = false;
        error.clear();
        return true;
    }
    lastRegion_ = region;
    hasRegion_  = true;
    // ROI 帧常驻复用，避免每帧 147KB 分配（AUDIT P2-8）。
    Camera::Frame& roi       = roiScratch_;
    roi.width                = 224U;
    roi.height               = 224U;
    roi.timestampNanoseconds = frame.timestampNanoseconds;
    roi.bytesPerRow          = 224U * 3U;
    roi.pixelFormat          = Camera::PixelFormat::RGB24;
    roi.orientation          = Camera::FrameOrientation::Up;
    roi.mirrored             = false;
    roi.pixels.resize(static_cast<std::size_t>(224U) * 224U * 3U);
    const float cosine = std::cos(region.rotation);
    const float sine   = std::sin(region.rotation);
    const bool  rotated =
        frame.orientation == Camera::FrameOrientation::Left || frame.orientation == Camera::FrameOrientation::Right;
    const std::uint32_t orientedWidth  = rotated ? frame.height : frame.width;
    const std::uint32_t orientedHeight = rotated ? frame.width : frame.height;
    for (std::uint32_t y = 0; y < roi.height; ++y) {
        for (std::uint32_t x = 0; x < roi.width; ++x) {
            const float   localX  = (static_cast<float>(x) + 0.5f) / static_cast<float>(roi.width) - 0.5f;
            const float   localY  = (static_cast<float>(y) + 0.5f) / static_cast<float>(roi.height) - 0.5f;
            const float   sourceX = region.handCenterX + region.handSide * (localX * cosine - localY * sine);
            const float   sourceY = region.handCenterY + region.handSide * (localX * sine + localY * cosine);
            std::uint32_t sampleX = static_cast<std::uint32_t>(std::clamp(sourceX, 0.0f, 1.0f) * (orientedWidth - 1U));
            const std::uint32_t sampleY =
                static_cast<std::uint32_t>(std::clamp(sourceY, 0.0f, 1.0f) * (orientedHeight - 1U));
            if (frame.mirrored) {
                sampleX = orientedWidth - 1U - sampleX;
            }
            std::uint32_t sourcePixelX = 0;
            std::uint32_t sourcePixelY = 0;
            MapOrientedPixel(frame, sampleX, sampleY, sourcePixelX, sourcePixelY);
            const std::uint32_t sourceBytesPerPixel = frame.pixelFormat == Camera::PixelFormat::BGRA32 ? 4U : 3U;
            const auto* source = frame.pixels.data() + static_cast<std::size_t>(sourcePixelY) * frame.bytesPerRow +
                                 static_cast<std::size_t>(sourcePixelX) * sourceBytesPerPixel;
            const std::uint32_t destinationX = region.isLeftHand ? roi.width - 1U - x : x;
            auto* destination = roi.pixels.data() + (static_cast<std::size_t>(y) * roi.width + destinationX) * 3U;
            if (frame.pixelFormat == Camera::PixelFormat::BGRA32) {
                destination[0] = source[2];
                destination[1] = source[1];
                destination[2] = source[0];
            } else {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
            }
        }
    }
    return landmark_.Invoke(roi, error);
}

bool XnnpackHandTrackingRuntime::IsLoaded() const noexcept {
    return palm_.IsLoaded() && landmark_.IsLoaded();
}

const std::vector<std::vector<float>>& XnnpackHandTrackingRuntime::PalmOutputs() const noexcept {
    return palm_.Outputs();
}

const std::vector<std::vector<float>>& XnnpackHandTrackingRuntime::LandmarkOutputs() const noexcept {
    return landmark_.Outputs();
}

bool XnnpackHandTrackingRuntime::DecodePalm(PalmRegion& region) const {
    const auto& outputs = palm_.Outputs();
    if (outputs.size() != 2U) {
        return false;
    }
    const auto* scores = outputs[0].size() == 2016U ? &outputs[0] : &outputs[1];
    const auto* boxes  = outputs[0].size() == 2016U * 18U ? &outputs[0] : &outputs[1];
    if (scores->size() != 2016U || boxes->size() != 2016U * 18U) {
        return false;
    }
    std::size_t best      = 0;
    float       bestScore = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < scores->size(); ++index) {
        if ((*scores)[index] > bestScore) {
            bestScore = (*scores)[index];
            best      = index;
        }
    }
    region.confidence = 1.0f / (1.0f + std::exp(-bestScore));
    if (region.confidence < 0.5f) {
        return false;
    }
    std::size_t remaining = best;
    float       anchorX   = 0.0f;
    float       anchorY   = 0.0f;
    for (const int stride : {8, 16}) {
        const std::size_t grid  = 192U / static_cast<std::size_t>(stride);
        const std::size_t count = grid * grid * (stride == 8 ? 2U : 6U);
        if (remaining < count) {
            const std::size_t cell = remaining / (stride == 8 ? 2U : 6U);
            anchorX                = (static_cast<float>(cell % grid) + 0.5f) / static_cast<float>(grid);
            anchorY                = (static_cast<float>(cell / grid) + 0.5f) / static_cast<float>(grid);
            break;
        }
        remaining -= count;
    }
    const float* box = boxes->data() + best * 18U;
    region.centerX   = box[0] / 192.0f + anchorX;
    region.centerY   = box[1] / 192.0f + anchorY;
    region.width     = box[2] / 192.0f;
    region.height    = box[3] / 192.0f;
    for (std::size_t index = 0; index < 7U; ++index) {
        region.keypoints[index * 2U]      = box[4U + index * 2U] / 192.0f + anchorX;
        region.keypoints[index * 2U + 1U] = box[5U + index * 2U] / 192.0f + anchorY;
    }
    region.rotation = static_cast<float>(M_PI_2) - std::atan2(-(region.keypoints[5] - region.keypoints[1]),
                                                              region.keypoints[4] - region.keypoints[0]);
    while (region.rotation > static_cast<float>(M_PI)) {
        region.rotation -= static_cast<float>(2.0 * M_PI);
    }
    while (region.rotation < -static_cast<float>(M_PI)) {
        region.rotation += static_cast<float>(2.0 * M_PI);
    }
    const float palmX  = region.keypoints[4] - region.keypoints[0];
    const float palmY  = region.keypoints[5] - region.keypoints[1];
    const float thumbX = region.keypoints[2] - region.keypoints[0];
    const float thumbY = region.keypoints[3] - region.keypoints[1];
    region.isLeftHand  = palmX * thumbY - palmY * thumbX > 0.0f;
    ExpandPalmToHandRegion(region);
    return true;
}

void XnnpackHandTrackingRuntime::ExpandPalmToHandRegion(PalmRegion& region) noexcept {
    constexpr float shiftY = -0.5f;
    region.handCenterX     = region.centerX - (region.height * shiftY) * std::sin(region.rotation);
    region.handCenterY     = region.centerY + (region.height * shiftY) * std::cos(region.rotation);
    region.handSide        = std::max(region.width, region.height) * 2.6f;
}

bool XnnpackHandTrackingRuntime::DecodeLandmarkOutputs(const std::vector<std::vector<float>>& outputs,
                                                       const PalmRegion& region, HandPose& pose) noexcept {
    const std::vector<float>* landmarks     = nullptr;
    float                     presence      = 0.0f;
    bool                      foundPresence = false;
    for (const auto& output : outputs) {
        if (output.size() == 63U && landmarks == nullptr) {
            landmarks = &output;
        }
        if (output.size() == 1U) {
            const float raw   = output[0];
            const float value = (raw < 0.0f || raw > 1.0f) ? 1.0f / (1.0f + std::exp(-raw)) : raw;
            if (!foundPresence || std::abs(raw) > 1.0f) {
                presence      = value;
                foundPresence = true;
            }
        }
    }
    if (landmarks == nullptr || !foundPresence || presence < 0.5f || region.handSide <= 0.0f) {
        return false;
    }
    float maximum = 0.0f;
    for (std::size_t index = 0; index < 21U; ++index) {
        maximum = std::max(maximum, std::abs((*landmarks)[index * 3U]));
        maximum = std::max(maximum, std::abs((*landmarks)[index * 3U + 1U]));
    }
    const float coordinateScale = maximum > 2.0f ? 1.0f / 224.0f : 1.0f;
    const float cosine          = std::cos(region.rotation);
    const float sine            = std::sin(region.rotation);
    const auto  transform       = [&](std::size_t index, float& x, float& y) {
        x = (*landmarks)[index * 3U] * coordinateScale;
        y = (*landmarks)[index * 3U + 1U] * coordinateScale;
        if (region.isLeftHand) {
            x = 1.0f - x;
        }
        const float localX = x - 0.5f;
        const float localY = y - 0.5f;
        x                  = region.handCenterX + region.handSide * (localX * cosine - localY * sine);
        y                  = region.handCenterY + region.handSide * (localX * sine + localY * cosine);
    };
    float wristX = 0.0f;
    float wristY = 0.0f;
    float thumbX = 0.0f;
    float thumbY = 0.0f;
    float indexX = 0.0f;
    float indexY = 0.0f;
    transform(0, wristX, wristY);
    transform(4, thumbX, thumbY);
    transform(8, indexX, indexY);
    pose.confidence                = presence;
    pose.centerX                   = std::clamp(wristX, 0.0f, 1.0f);
    pose.centerY                   = std::clamp(wristY, 0.0f, 1.0f);
    const float distance           = std::hypot(thumbX - indexX, thumbY - indexY);
    const float normalizedDistance = std::clamp((distance - 0.02f) / 0.25f, 0.0f, 1.0f);
    pose.scale                     = 0.5f + normalizedDistance * 2.0f;
    return true;
}

bool XnnpackHandTrackingRuntime::DecodeLandmarks(HandPose& pose) const {
    return hasRegion_ && DecodeLandmarkOutputs(landmark_.Outputs(), lastRegion_, pose);
}

} // namespace ParticleSaturn::Services::HandTracking::MacOS
