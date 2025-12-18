#include "HandLandmark.h"

#include <algorithm>
#include <fstream>
#include <iostream>

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

HandLandmark::HandLandmark() {}

HandLandmark::~HandLandmark() {}

bool HandLandmark::buildInterpreter() {
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder              builder(*model, resolver);

    if (builder(&interpreter) != kTfLiteOk) {
        return false;
    }

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        return false;
    }

    return true;
}

bool HandLandmark::load(const std::string& model_path) {
    model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!model) {
        return false;
    }

    if (!buildInterpreter()) {
        return false;
    }

    return true;
}

bool HandLandmark::loadFromMemory(const void* data, size_t size) {
    model_buffer.assign((const char*)data, (const char*)data + size);
    model = tflite::FlatBufferModel::BuildFromBuffer(model_buffer.data(), model_buffer.size());

    if (!model) {
        return false;
    }

    if (!buildInterpreter()) {
        return false;
    }

    return true;
}

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

float HandLandmark::detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv, std::vector<cv::Point2f>& landmarks,
                           bool is_left_hand) {
    landmarks.clear();

    if (roi_image.empty() || !interpreter) {
        return 0.0f;
    }

    // Resize input (always from original roi_image)
    cv::Mat resized;
    cv::resize(roi_image, resized, cv::Size(input_size, input_size));

    // Get input tensor
    int    input_idx    = interpreter->inputs()[0];
    float* input_tensor = interpreter->typed_tensor<float>(input_idx);

    // Optimize: Direct copy, normalize (and optionally flip) to float tensor
    // This avoids cv::flip (allocation), convertTo (allocation), and memcpy
    for (int y = 0; y < input_size; ++y) {
        const uint8_t* row_ptr     = resized.ptr<uint8_t>(y);
        float*         dst_row_ptr = input_tensor + y * input_size * 3;

        if (is_left_hand) {
            // Horizontal flip on-the-fly
            for (int x = 0; x < input_size; ++x) {
                int src_idx            = (input_size - 1 - x) * 3;
                dst_row_ptr[x * 3 + 0] = row_ptr[src_idx + 0] * (1.0f / 255.0f);
                dst_row_ptr[x * 3 + 1] = row_ptr[src_idx + 1] * (1.0f / 255.0f);
                dst_row_ptr[x * 3 + 2] = row_ptr[src_idx + 2] * (1.0f / 255.0f);
            }
        } else {
            // Direct copy
            for (int x = 0; x < input_size; ++x) {
                int src_idx            = x * 3;
                dst_row_ptr[x * 3 + 0] = row_ptr[src_idx + 0] * (1.0f / 255.0f);
                dst_row_ptr[x * 3 + 1] = row_ptr[src_idx + 1] * (1.0f / 255.0f);
                dst_row_ptr[x * 3 + 2] = row_ptr[src_idx + 2] * (1.0f / 255.0f);
            }
        }
    }

    // Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        return 0.0f;
    }

    // Parse outputs
    // MediaPipe hand_landmark_full.tflite outputs (order may vary):
    // - size=63: screen_landmarks (21 points * 3 coords)
    // - size=1: handedness (0=left, 1=right)
    // - size=1: hand_presence score
    // - size=63: world_landmarks
    float                              hand_presence   = 1.0f;
    const float*                       landmarks_data  = nullptr;
    bool                               found_landmarks = false;
    std::vector<std::pair<int, float>> single_outputs;

    for (size_t i = 0; i < interpreter->outputs().size(); i++) {
        int           idx    = interpreter->outputs()[i];
        TfLiteTensor* tensor = interpreter->tensor(idx);
        int           total  = 1;
        for (int d = 0; d < tensor->dims->size; d++) {
            total *= tensor->dims->data[d];
        }

        const float* data = interpreter->typed_output_tensor<float>((int)i);

        if (total == 63 && !found_landmarks) {
            landmarks_data  = data;
            found_landmarks = true;
        } else if (total == 1) {
            single_outputs.push_back({(int)i, data[0]});
        }
    }

    // Find hand_presence from single outputs
    if (!single_outputs.empty()) {
        float best_presence = 0.0f;
        for (auto& p : single_outputs) {
            float raw = p.second;
            float val = raw;
            if (raw < 0.0f || raw > 1.0f) {
                val = sigmoid(raw);
            }
            if (std::abs(raw) > 1.0f || best_presence == 0.0f) {
                best_presence = val;
            }
        }
        hand_presence = best_presence;
    }

    if (!landmarks_data) {
        return 0.0f;
    }

    // Check coordinate format by examining max values
    float max_val = 0.0f;
    for (int i = 0; i < 21; i++) {
        float x = landmarks_data[i * 3];
        float y = landmarks_data[i * 3 + 1];
        max_val = std::max(max_val, std::max(std::abs(x), std::abs(y)));
    }

    // Determine if coordinates are normalized (0~1) or pixel (0~224)
    bool is_pixel_coords = (max_val > 2.0f);

    // Extract 21 landmarks
    for (int i = 0; i < 21; i++) {
        float x = landmarks_data[i * 3];
        float y = landmarks_data[i * 3 + 1];

        // Convert to pixel coordinates in ROI space (0~input_size)
        if (!is_pixel_coords) {
            x *= input_size;
            y *= input_size;
        }

        // 如果是左手，需要将x坐标翻转回来
        if (is_left_hand) {
            x = input_size - x;
        }

        // Transform back to original image coordinates using inverse affine matrix
        cv::Point2f pt;
        pt.x = (float)(x * trans_mat_inv.at<double>(0, 0) + y * trans_mat_inv.at<double>(0, 1) +
                       trans_mat_inv.at<double>(0, 2));
        pt.y = (float)(x * trans_mat_inv.at<double>(1, 0) + y * trans_mat_inv.at<double>(1, 1) +
                       trans_mat_inv.at<double>(1, 2));

        landmarks.push_back(pt);
    }

    return hand_presence;
}

// 兼容旧接口
float HandLandmark::detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv,
                           std::vector<cv::Point2f>& landmarks) {
    return detect(roi_image, trans_mat_inv, landmarks, false);
}
