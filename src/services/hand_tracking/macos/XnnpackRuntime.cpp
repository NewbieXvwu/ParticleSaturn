#include "XnnpackRuntime.h"

#include "tensorflow/lite/core/c/c_api.h"
#include "tensorflow/lite/delegates/xnnpack/xnnpack_delegate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ParticleSaturn::Services::HandTracking::MacOS {

XnnpackModel::~XnnpackModel() { Reset(); }

void XnnpackModel::Reset() noexcept {
    if (interpreter_ != nullptr) TfLiteInterpreterDelete(interpreter_);
    if (delegate_ != nullptr) TfLiteXNNPackDelegateDelete(delegate_);
    if (model_ != nullptr) TfLiteModelDelete(model_);
    interpreter_ = nullptr;
    delegate_ = nullptr;
    model_ = nullptr;
    inputWidth_ = 0;
    inputHeight_ = 0;
    outputs_.clear();
}

bool XnnpackModel::Load(const std::string& modelPath, std::string& error) {
    Reset();
    model_ = TfLiteModelCreateFromFile(modelPath.c_str());
    if (model_ == nullptr) {
        error = "unable to load TensorFlow Lite model: " + modelPath;
        return false;
    }
    TfLiteXNNPackDelegateOptions delegateOptions = TfLiteXNNPackDelegateOptionsDefault();
    delegateOptions.num_threads = 0;
    delegate_ = TfLiteXNNPackDelegateCreate(&delegateOptions);
    if (delegate_ == nullptr) {
        error = "unable to create XNNPACK delegate";
        Reset();
        return false;
    }
    TfLiteInterpreterOptions* options = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(options, 0);
    TfLiteInterpreterOptionsAddDelegate(options, delegate_);
    interpreter_ = TfLiteInterpreterCreate(model_, options);
    TfLiteInterpreterOptionsDelete(options);
    if (interpreter_ == nullptr || TfLiteInterpreterAllocateTensors(interpreter_) != kTfLiteOk) {
        error = "unable to allocate XNNPACK interpreter tensors";
        Reset();
        return false;
    }
    TfLiteTensor* input = TfLiteInterpreterGetInputTensor(interpreter_, 0);
    if (input == nullptr || TfLiteTensorType(input) != kTfLiteFloat32 || TfLiteTensorNumDims(input) != 4 ||
        TfLiteTensorDim(input, 0) != 1 || TfLiteTensorDim(input, 3) != 3) {
        error = "hand model must expose one NHWC float32 RGB input";
        Reset();
        return false;
    }
    inputHeight_ = TfLiteTensorDim(input, 1);
    inputWidth_ = TfLiteTensorDim(input, 2);
    if (inputWidth_ <= 0 || inputHeight_ <= 0) {
        error = "hand model input dimensions are invalid";
        Reset();
        return false;
    }
    error.clear();
    return true;
}

bool XnnpackModel::Invoke(const Camera::Frame& frame, std::string& error) {
    if (!IsLoaded()) {
        error = "XNNPACK model is not loaded";
        return false;
    }
    const std::size_t sourceBytes = static_cast<std::size_t>(frame.width) * frame.height * 3U;
    if (frame.width == 0 || frame.height == 0 || frame.rgb.size() != sourceBytes) {
        error = "camera frame is not tightly packed RGB";
        return false;
    }
    std::vector<float> input(static_cast<std::size_t>(inputWidth_) * inputHeight_ * 3U);
    for (int y = 0; y < inputHeight_; ++y) {
        const std::uint32_t sourceY = std::min(frame.height - 1U, static_cast<std::uint32_t>(y) * frame.height / inputHeight_);
        for (int x = 0; x < inputWidth_; ++x) {
            const std::uint32_t sourceX = std::min(frame.width - 1U, static_cast<std::uint32_t>(x) * frame.width / inputWidth_);
            const auto* source = frame.rgb.data() + (static_cast<std::size_t>(sourceY) * frame.width + sourceX) * 3U;
            auto* target = input.data() + (static_cast<std::size_t>(y) * inputWidth_ + x) * 3U;
            target[0] = source[0] * (1.0f / 255.0f);
            target[1] = source[1] * (1.0f / 255.0f);
            target[2] = source[2] * (1.0f / 255.0f);
        }
    }
    TfLiteTensor* tensor = TfLiteInterpreterGetInputTensor(interpreter_, 0);
    if (TfLiteTensorCopyFromBuffer(tensor, input.data(), input.size() * sizeof(float)) != kTfLiteOk ||
        TfLiteInterpreterInvoke(interpreter_) != kTfLiteOk) {
        error = "XNNPACK model invocation failed";
        return false;
    }
    outputs_.clear();
    const int outputCount = TfLiteInterpreterGetOutputTensorCount(interpreter_);
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
        std::vector<float> values(elementCount);
        if (TfLiteTensorCopyToBuffer(output, values.data(), values.size() * sizeof(float)) != kTfLiteOk) {
            error = "unable to read hand model output";
            outputs_.clear();
            return false;
        }
        outputs_.push_back(std::move(values));
    }
    error.clear();
    return true;
}

bool XnnpackModel::IsLoaded() const noexcept { return interpreter_ != nullptr; }

const std::vector<std::vector<float>>& XnnpackModel::Outputs() const noexcept { return outputs_; }

bool XnnpackHandTrackingRuntime::Load(const std::string& palmModelPath, const std::string& landmarkModelPath, std::string& error) {
    if (!palm_.Load(palmModelPath, error)) return false;
    if (!landmark_.Load(landmarkModelPath, error)) return false;
    return true;
}

bool XnnpackHandTrackingRuntime::Invoke(const Camera::Frame& frame, std::string& error) {
    return palm_.Invoke(frame, error) && landmark_.Invoke(frame, error);
}

bool XnnpackHandTrackingRuntime::IsLoaded() const noexcept { return palm_.IsLoaded() && landmark_.IsLoaded(); }

} // namespace ParticleSaturn::Services::HandTracking::MacOS
