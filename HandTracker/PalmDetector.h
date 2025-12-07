#pragma once
#ifndef PALM_DETECTOR_H
#define PALM_DETECTOR_H

#include <opencv2/opencv.hpp>
#include <opencv2/dnn.hpp>
#include <vector>

// 手掌检测结果
struct PalmDetection {
    float score;                // 置信度
    cv::Rect2f rect;            // 手掌边界框（归一化坐标 0~1）
    cv::Point2f landmarks[7];   // 7 个关键点（归一化坐标）
    float rotation;             // 旋转角度
    
    // 扩展的手部 ROI 信息（用于后续关键点检测）
    float hand_cx, hand_cy;     // 手部中心（归一化）
    float hand_w, hand_h;       // 手部区域大小（归一化）
    cv::Point2f hand_pos[4];    // ROI 四个角点（归一化）
};

// 手掌检测器 - 基于 MediaPipe Palm Detection 模型
class PalmDetector {
public:
    PalmDetector();
    ~PalmDetector();
    
    bool load(const std::string& model_path);
    bool loadFromMemory(const void* data, size_t size);
    
    // 检测图像中的手掌
    // image: 输入图像（BGR 或 RGB）
    // prob_threshold: 置信度阈值
    // nms_threshold: NMS 阈值
    std::vector<PalmDetection> detect(const cv::Mat& image, float prob_threshold = 0.5f, float nms_threshold = 0.3f);

private:
    cv::dnn::Net net;
    int input_size = 192;
    
    struct Anchor { float x_center, y_center; };
    std::vector<Anchor> anchors;
    
    void generateAnchors();
    void decodeDetections(const cv::Mat& scores, const cv::Mat& boxes, 
                          std::vector<PalmDetection>& detections, float threshold);
    void computeRotation(PalmDetection& det);
    void convertToHandROI(PalmDetection& det);
};

#endif
