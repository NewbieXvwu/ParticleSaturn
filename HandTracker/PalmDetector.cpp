#define _USE_MATH_DEFINES
#include "PalmDetector.h"

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/kernels/register.h"
#include "tensorflow/lite/model.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>

PalmDetector::PalmDetector() {}

PalmDetector::~PalmDetector() {}

bool PalmDetector::buildInterpreter() {
    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder              builder(*model, resolver);

    if (builder(&interpreter) != kTfLiteOk) {
        std::cerr << "[PalmDetector] Failed to build interpreter" << std::endl;
        return false;
    }

    if (interpreter->AllocateTensors() != kTfLiteOk) {
        std::cerr << "[PalmDetector] Failed to allocate tensors" << std::endl;
        return false;
    }

    generateAnchors();
    return true;
}

bool PalmDetector::load(const std::string& model_path) {
    model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!model) {
        std::cerr << "[PalmDetector] Failed to load model: " << model_path << std::endl;
        return false;
    }

    if (!buildInterpreter()) {
        return false;
    }

    std::cout << "[PalmDetector] Model loaded: " << model_path << std::endl;
    return true;
}

bool PalmDetector::loadFromMemory(const void* data, size_t size) {
    model_buffer.assign((const char*)data, (const char*)data + size);
    model = tflite::FlatBufferModel::BuildFromBuffer(model_buffer.data(), model_buffer.size());

    if (!model) {
        std::cerr << "[PalmDetector] Failed to load model from memory" << std::endl;
        return false;
    }

    if (!buildInterpreter()) {
        return false;
    }

    std::cout << "[PalmDetector] Model loaded from memory (" << size << " bytes)" << std::endl;
    return true;
}

// Generate SSD-style anchor boxes
void PalmDetector::generateAnchors() {
    anchors.clear();
    std::vector<int> strides  = {8, 16, 16, 16};
    int              layer_id = 0;

    while (layer_id < (int)strides.size()) {
        int last_same = layer_id;
        while (last_same < (int)strides.size() && strides[last_same] == strides[layer_id]) {
            last_same++;
        }
        int num_anchors_per_loc = 2 * (last_same - layer_id);
        int stride              = strides[layer_id];
        int grid_size           = input_size / stride;

        for (int y = 0; y < grid_size; y++) {
            for (int x = 0; x < grid_size; x++) {
                for (int n = 0; n < num_anchors_per_loc; n++) {
                    Anchor a;
                    a.x_center = (x + 0.5f) / grid_size;
                    a.y_center = (y + 0.5f) / grid_size;
                    anchors.push_back(a);
                }
            }
        }
        layer_id = last_same;
    }
    std::cout << "[PalmDetector] Generated " << anchors.size() << " anchors" << std::endl;
}

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

// 计算两个矩形的 IoU (Intersection over Union)
static float computeIoU(const cv::Rect& a, const cv::Rect& b) {
    int x1 = std::max(a.x, b.x);
    int y1 = std::max(a.y, b.y);
    int x2 = std::min(a.x + a.width, b.x + b.width);
    int y2 = std::min(a.y + a.height, b.y + b.height);

    int interArea = std::max(0, x2 - x1) * std::max(0, y2 - y1);
    int unionArea = a.width * a.height + b.width * b.height - interArea;

    return unionArea > 0 ? (float)interArea / unionArea : 0.0f;
}

// 非极大值抑制 (NMS)
static void NMSBoxes(const std::vector<cv::Rect>& boxes, const std::vector<float>& scores, float scoreThreshold,
                     float nmsThreshold, std::vector<int>& indices) {
    indices.clear();

    // 按置信度降序排列的索引
    std::vector<int> order(scores.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&scores](int i, int j) { return scores[i] > scores[j]; });

    std::vector<bool> suppressed(boxes.size(), false);

    for (int i : order) {
        if (suppressed[i] || scores[i] < scoreThreshold) {
            continue;
        }

        indices.push_back(i);

        for (int j : order) {
            if (suppressed[j] || i == j) {
                continue;
            }
            if (computeIoU(boxes[i], boxes[j]) > nmsThreshold) {
                suppressed[j] = true;
            }
        }
    }
}

// Decode detections (from anchor offsets to actual coordinates)
void PalmDetector::decodeDetections(const float* scores, const float* boxes, int num_anchors,
                                    std::vector<PalmDetection>& detections, float threshold) {
    for (int i = 0; i < num_anchors && i < (int)anchors.size(); i++) {
        float score = sigmoid(scores[i]);
        if (score < threshold) {
            continue;
        }

        const float*  p      = boxes + i * 18;
        const Anchor& anchor = anchors[i];

        float cx = p[0] / input_size + anchor.x_center;
        float cy = p[1] / input_size + anchor.y_center;
        float w  = p[2] / input_size;
        float h  = p[3] / input_size;

        PalmDetection det;
        det.score = score;
        det.rect  = cv::Rect2f(cx - w / 2, cy - h / 2, w, h);

        for (int j = 0; j < 7; j++) {
            float lx         = p[4 + j * 2] / input_size + anchor.x_center;
            float ly         = p[4 + j * 2 + 1] / input_size + anchor.y_center;
            det.landmarks[j] = cv::Point2f(lx, ly);
        }

        detections.push_back(det);
    }
}

// Compute palm rotation angle (based on keypoints 0 and 2)
void PalmDetector::computeRotation(PalmDetection& det) {
    float x0 = det.landmarks[0].x;
    float y0 = det.landmarks[0].y;
    float x2 = det.landmarks[2].x;
    float y2 = det.landmarks[2].y;

    float target_angle = (float)M_PI * 0.5f;
    float rotation     = target_angle - std::atan2(-(y2 - y0), x2 - x0);

    while (rotation > M_PI) {
        rotation -= 2 * (float)M_PI;
    }
    while (rotation < -M_PI) {
        rotation += 2 * (float)M_PI;
    }

    det.rotation = rotation;
}

// Expand palm detection box to hand ROI (for subsequent landmark detection)
void PalmDetector::convertToHandROI(PalmDetection& det) {
    float w  = det.rect.width;
    float h  = det.rect.height;
    float cx = det.rect.x + w * 0.5f;
    float cy = det.rect.y + h * 0.5f;

    float rotation = det.rotation;
    float shift_y  = -0.5f; // shift towards wrist

    float dx = -(h * shift_y) * std::sin(rotation);
    float dy = (h * shift_y) * std::cos(rotation);

    det.hand_cx = cx + dx;
    det.hand_cy = cy + dy;

    float long_side = std::max(w, h);
    det.hand_w      = long_side * 2.6f; // expand 2.6x to include entire hand
    det.hand_h      = long_side * 2.6f;

    float half_w = det.hand_w * 0.5f;
    float half_h = det.hand_h * 0.5f;

    cv::Point2f corners[4] = {cv::Point2f(-half_w, -half_h), cv::Point2f(half_w, -half_h), cv::Point2f(half_w, half_h),
                              cv::Point2f(-half_w, half_h)};

    // Rotate four corners
    float cos_r = std::cos(rotation);
    float sin_r = std::sin(rotation);

    for (int i = 0; i < 4; i++) {
        float rx        = corners[i].x * cos_r - corners[i].y * sin_r;
        float ry        = corners[i].x * sin_r + corners[i].y * cos_r;
        det.hand_pos[i] = cv::Point2f(det.hand_cx + rx, det.hand_cy + ry);
    }
}

std::vector<PalmDetection> PalmDetector::detect(const cv::Mat& image, float prob_threshold, float nms_threshold) {
    std::vector<PalmDetection> results;

    if (image.empty() || !interpreter) {
        return results;
    }

    // Resize input
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(input_size, input_size));

    // Get input tensor
    int    input_idx    = interpreter->inputs()[0];
    float* input_tensor = interpreter->typed_tensor<float>(input_idx);

    // Copy image data to input tensor (normalize to 0~1, NHWC format)
    cv::Mat float_img;
    resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
    memcpy(input_tensor, float_img.data, input_size * input_size * 3 * sizeof(float));

    // Run inference
    if (interpreter->Invoke() != kTfLiteOk) {
        std::cerr << "[PalmDetector] Inference failed" << std::endl;
        return results;
    }

    // Parse outputs (scores and boxes)
    const float* scores     = nullptr;
    const float* boxes      = nullptr;
    int          num_scores = 0;

    for (int i = 0; i < interpreter->outputs().size(); i++) {
        int           idx    = interpreter->outputs()[i];
        TfLiteTensor* tensor = interpreter->tensor(idx);
        int           total  = 1;
        for (int d = 0; d < tensor->dims->size; d++) {
            total *= tensor->dims->data[d];
        }

        const float* data = interpreter->typed_output_tensor<float>(i);

        if (total == (int)anchors.size()) {
            scores     = data;
            num_scores = total;
        } else if (total == (int)anchors.size() * 18) {
            boxes = data;
        }
    }

    if (!scores || !boxes) {
        std::cerr << "[PalmDetector] Failed to parse outputs" << std::endl;
        return results;
    }

    std::vector<PalmDetection> candidates;
    decodeDetections(scores, boxes, num_scores, candidates, prob_threshold);

    if (candidates.empty()) {
        return results;
    }

    // NMS (Non-Maximum Suppression)
    std::vector<cv::Rect> rects;
    std::vector<float>    confidences;
    for (auto& det : candidates) {
        rects.push_back(cv::Rect((int)(det.rect.x * 1000), (int)(det.rect.y * 1000), (int)(det.rect.width * 1000),
                                 (int)(det.rect.height * 1000)));
        confidences.push_back(det.score);
    }

    std::vector<int> indices;
    NMSBoxes(rects, confidences, prob_threshold, nms_threshold, indices);

    // Process NMS results
    for (int idx : indices) {
        PalmDetection& det = candidates[idx];
        computeRotation(det);
        convertToHandROI(det);
        results.push_back(det);
        if (results.size() >= 2) {
            break;
        }
    }

    return results;
}
