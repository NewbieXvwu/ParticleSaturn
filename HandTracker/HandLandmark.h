#pragma once
#ifndef HAND_LANDMARK_H
#define HAND_LANDMARK_H

#include <opencv2/dnn.hpp>
#include <opencv2/opencv.hpp>
#include <vector>

// 手部关键点检测器 - 基于 MediaPipe Hand Landmark 模型
class HandLandmark {
  public:
    HandLandmark();
    ~HandLandmark();

    bool load(const std::string& model_path);
    bool loadFromMemory(const void* data, size_t size);

    // 检测手部 21 个关键点
    // roi_image: 224x224 手部 ROI 图像
    // trans_mat_inv: 逆仿射变换矩阵，用于将坐标还原到原图
    // landmarks: 输出 21 个关键点坐标（原图坐标系）
    // 返回值: 手部存在置信度 (0.0~1.0)
    float detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv, std::vector<cv::Point2f>& landmarks);

  private:
    cv::dnn::Net net;
    int          input_size = 224;
};

#endif
