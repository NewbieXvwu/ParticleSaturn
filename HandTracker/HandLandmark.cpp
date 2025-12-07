#include "HandLandmark.h"
#include <iostream>
#include <algorithm>

HandLandmark::HandLandmark() {}
HandLandmark::~HandLandmark() {}

bool HandLandmark::load(const std::string& model_path) {
    try {
        net = cv::dnn::readNetFromONNX(model_path);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        std::cout << "[HandLandmark] Model loaded: " << model_path << std::endl;
        return true;
    } catch (const cv::Exception& e) {
        std::cerr << "[HandLandmark] Failed to load model: " << e.what() << std::endl;
        return false;
    }
}

bool HandLandmark::loadFromMemory(const void* data, size_t size) {
    try {
        std::vector<uchar> buffer((const uchar*)data, (const uchar*)data + size);
        net = cv::dnn::readNetFromONNX(buffer);
        net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
        net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
        std::cout << "[HandLandmark] Model loaded from memory (" << size << " bytes)" << std::endl;
        return true;
    } catch (const cv::Exception& e) {
        std::cerr << "[HandLandmark] Failed to load model from memory: " << e.what() << std::endl;
        return false;
    }
}

static inline float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

float HandLandmark::detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv, std::vector<cv::Point2f>& landmarks) {
    landmarks.clear();
    
    if (roi_image.empty()) return 0.0f;
    
    cv::Mat resized;
    cv::resize(roi_image, resized, cv::Size(input_size, input_size));
    cv::Mat blob = cv::dnn::blobFromImage(resized, 1.0/255.0, cv::Size(),
                                           cv::Scalar(0, 0, 0), false, false);
    
    net.setInput(blob);
    
    std::vector<cv::String> outNames = net.getUnconnectedOutLayersNames();
    std::vector<cv::Mat> outputs;
    net.forward(outputs, outNames);
    
    cv::Mat landmarks_out;
    float hand_presence = 1.0f;
    
    // 调试：打印输出层信息（仅一次）
    static bool printed = false;
    if (!printed) {
        std::cout << "[HandLandmark] Output layers: " << outputs.size() << std::endl;
        for (size_t i = 0; i < outputs.size(); i++) {
            std::cout << "  Output " << i << ": total=" << outputs[i].total() 
                      << ", dims=" << outputs[i].dims;
            if (outputs[i].total() == 63) {
                const float* d = (const float*)outputs[i].data;
                std::cout << " first 6 vals: [" << d[0] << "," << d[1] << "," << d[2] 
                          << "," << d[3] << "," << d[4] << "," << d[5] << "]";
            }
            std::cout << std::endl;
        }
        printed = true;
    }
    
    // 查找关键点输出层（63 个值 = 21 个点 × 3 坐标）
    int landmarks_idx = -1;
    for (size_t i = 0; i < outputs.size(); i++) {
        if (outputs[i].total() == 63) {
            const float* d = (const float*)outputs[i].data;
            float x0 = d[0], y0 = d[1];
            if (x0 > 1.0f || y0 > 1.0f) {
                landmarks_idx = (int)i;
                break;
            }
        }
    }
    
    if (landmarks_idx < 0) {
        for (size_t i = 0; i < outputs.size(); i++) {
            if (outputs[i].total() == 63) {
                landmarks_idx = (int)i;
                break;
            }
        }
    }
    
    if (landmarks_idx >= 0) {
        landmarks_out = outputs[landmarks_idx];
    }
    
    // 查找手部存在置信度输出
    for (size_t i = 0; i < outputs.size(); i++) {
        if (outputs[i].total() == 1) {
            float val = ((float*)outputs[i].data)[0];
            if (val < 0.0f || val > 1.0f) {
                val = sigmoid(val);
            }
            if (val < hand_presence) {
                hand_presence = val;
            }
        }
    }
    
    if (landmarks_out.empty()) {
        std::cerr << "[HandLandmark] No landmarks output found" << std::endl;
        return 0.0f;
    }
    
    const float* data = (const float*)landmarks_out.data;
    
    // 检测坐标格式（归一化 0~1 或像素坐标）
    float max_val = 0.0f;
    float min_val = 1e9f;
    for (int i = 0; i < 42; i++) {
        int idx = (i / 2) * 3 + (i % 2);
        max_val = std::max(max_val, data[idx]);
        min_val = std::min(min_val, data[idx]);
    }
    
    bool is_pixel_coords = (max_val > 2.0f);
    
    for (int i = 0; i < 21; i++) {
        float x = data[i * 3];
        float y = data[i * 3 + 1];
        
        if (!is_pixel_coords) {
            x *= input_size;
            y *= input_size;
        }
        
        cv::Point2f pt;
        pt.x = (float)(x * trans_mat_inv.at<double>(0, 0) + y * trans_mat_inv.at<double>(0, 1) + trans_mat_inv.at<double>(0, 2));
        pt.y = (float)(x * trans_mat_inv.at<double>(1, 0) + y * trans_mat_inv.at<double>(1, 1) + trans_mat_inv.at<double>(1, 2));
        
        landmarks.push_back(pt);
    }
    
    return hand_presence;
}
