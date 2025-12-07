#define _USE_MATH_DEFINES
#include "PalmDetector.h"
#include <cmath>
#include <algorithm>
#include <iostream>

PalmDetector::PalmDetector() {}
PalmDetector::~PalmDetector() {}

bool PalmDetector::load(const std::string& model_path) {
    try {
        net = cv::dnn::readNetFromONNX(model_path);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        generateAnchors();
        std::cout << "[PalmDetector] Model loaded: " << model_path << std::endl;
        return true;
    } catch (const cv::Exception& e) {
        std::cerr << "[PalmDetector] Failed to load model: " << e.what() << std::endl;
        return false;
    }
}

bool PalmDetector::loadFromMemory(const void* data, size_t size) {
    try {
        std::vector<uchar> buffer((const uchar*)data, (const uchar*)data + size);
        net = cv::dnn::readNetFromONNX(buffer);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        generateAnchors();
        std::cout << "[PalmDetector] Model loaded from memory (" << size << " bytes)" << std::endl;
        return true;
    } catch (const cv::Exception& e) {
        std::cerr << "[PalmDetector] Failed to load model from memory: " << e.what() << std::endl;
        return false;
    }
}

// 生成 SSD 风格的 anchor boxes
void PalmDetector::generateAnchors() {
    anchors.clear();
    std::vector<int> strides = {8, 16, 16, 16};
    int layer_id = 0;
    
    while (layer_id < (int)strides.size()) {
        int last_same = layer_id;
        while (last_same < (int)strides.size() && strides[last_same] == strides[layer_id]) {
            last_same++;
        }
        int num_anchors_per_loc = 2 * (last_same - layer_id);
        int stride = strides[layer_id];
        int grid_size = input_size / stride;
        
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

// 解码检测结果（从 anchor 偏移量到实际坐标）
void PalmDetector::decodeDetections(const cv::Mat& scores, const cv::Mat& boxes,
                                     std::vector<PalmDetection>& detections, float threshold) {
    const float* score_data = (const float*)scores.data;
    const float* box_data = (const float*)boxes.data;
    
    for (size_t i = 0; i < anchors.size(); i++) {
        float score = sigmoid(score_data[i]);
        if (score < threshold) continue;
        
        const float* p = box_data + i * 18;
        const Anchor& anchor = anchors[i];
        
        float cx = p[0] / input_size + anchor.x_center;
        float cy = p[1] / input_size + anchor.y_center;
        float w = p[2] / input_size;
        float h = p[3] / input_size;
        
        PalmDetection det;
        det.score = score;
        det.rect = cv::Rect2f(cx - w/2, cy - h/2, w, h);
        
        for (int j = 0; j < 7; j++) {
            float lx = p[4 + j*2] / input_size + anchor.x_center;
            float ly = p[4 + j*2 + 1] / input_size + anchor.y_center;
            det.landmarks[j] = cv::Point2f(lx, ly);
        }
        
        detections.push_back(det);
    }
}

// 计算手掌旋转角度（基于关键点 0 和 2）
void PalmDetector::computeRotation(PalmDetection& det) {
    float x0 = det.landmarks[0].x;
    float y0 = det.landmarks[0].y;
    float x2 = det.landmarks[2].x;
    float y2 = det.landmarks[2].y;
    
    float target_angle = (float)M_PI * 0.5f;
    float rotation = target_angle - std::atan2(-(y2 - y0), x2 - x0);
    
    while (rotation > M_PI) rotation -= 2 * (float)M_PI;
    while (rotation < -M_PI) rotation += 2 * (float)M_PI;
    
    det.rotation = rotation;
}

// 将手掌检测框扩展为手部 ROI（用于后续关键点检测）
void PalmDetector::convertToHandROI(PalmDetection& det) {
    float w = det.rect.width;
    float h = det.rect.height;
    float cx = det.rect.x + w * 0.5f;
    float cy = det.rect.y + h * 0.5f;
    
    float rotation = det.rotation;
    float shift_y = -0.5f;  // 向手腕方向偏移
    
    float dx = -(h * shift_y) * std::sin(rotation);
    float dy = (h * shift_y) * std::cos(rotation);
    
    det.hand_cx = cx + dx;
    det.hand_cy = cy + dy;
    
    float long_side = std::max(w, h);
    det.hand_w = long_side * 2.6f;  // 扩大 2.6 倍以包含整个手部
    det.hand_h = long_side * 2.6f;
    
    float half_w = det.hand_w * 0.5f;
    float half_h = det.hand_h * 0.5f;
    
    cv::Point2f corners[4] = {
        cv::Point2f(-half_w, -half_h),
        cv::Point2f(half_w, -half_h),
        cv::Point2f(half_w, half_h),
        cv::Point2f(-half_w, half_h)
    };
    
    // 旋转四个角点
    float cos_r = std::cos(rotation);
    float sin_r = std::sin(rotation);
    
    for (int i = 0; i < 4; i++) {
        float rx = corners[i].x * cos_r - corners[i].y * sin_r;
        float ry = corners[i].x * sin_r + corners[i].y * cos_r;
        det.hand_pos[i] = cv::Point2f(det.hand_cx + rx, det.hand_cy + ry);
    }
}

std::vector<PalmDetection> PalmDetector::detect(const cv::Mat& image, float prob_threshold, float nms_threshold) {
    std::vector<PalmDetection> results;
    
    if (image.empty()) return results;
    
    cv::Mat resized;
    cv::resize(image, resized, cv::Size(input_size, input_size));
    cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0/255.0, cv::Size(), 
                                           cv::Scalar(0, 0, 0), false, false);
    
    net.setInput(blob);
    
    std::vector<cv::String> outNames = net.getUnconnectedOutLayersNames();
    std::vector<cv::Mat> outputs;
    net.forward(outputs, outNames);
    
    // 解析输出层（scores 和 boxes）
    cv::Mat scores, boxes;
    for (auto& out : outputs) {
        if (out.size[2] == 1 || out.total() == anchors.size()) {
            scores = out.reshape(1, {1, (int)anchors.size(), 1});
        } else {
            boxes = out.reshape(1, {1, (int)anchors.size(), 18});
        }
    }
    
    if (scores.empty() || boxes.empty()) {
        std::cerr << "[PalmDetector] Failed to parse outputs" << std::endl;
        return results;
    }
    
    std::vector<PalmDetection> candidates;
    decodeDetections(scores, boxes, candidates, prob_threshold);
    
    if (candidates.empty()) return results;
    
    // NMS（非极大值抑制）
    std::vector<cv::Rect> rects;
    std::vector<float> confidences;
    for (auto& det : candidates) {
        rects.push_back(cv::Rect(
            (int)(det.rect.x * 1000), (int)(det.rect.y * 1000),
            (int)(det.rect.width * 1000), (int)(det.rect.height * 1000)
        ));
        confidences.push_back(det.score);
    }
    
    std::vector<int> indices;
    cv::dnn::NMSBoxes(rects, confidences, prob_threshold, nms_threshold, indices);
    
    // 处理 NMS 后的结果
    for (int idx : indices) {
        PalmDetection& det = candidates[idx];
        computeRotation(det);
        convertToHandROI(det);
        results.push_back(det);
        if (results.size() >= 2) break;
    }
    
    return results;
}
