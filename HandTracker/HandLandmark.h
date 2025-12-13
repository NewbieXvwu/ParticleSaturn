#pragma once
#ifndef HAND_LANDMARK_H
#define HAND_LANDMARK_H

#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/model.h"

// Hand landmark detector - based on MediaPipe Hand Landmark model
class HandLandmark {
  public:
    HandLandmark();
    ~HandLandmark();

    bool load(const std::string& model_path);
    bool loadFromMemory(const void* data, size_t size);

    // Detect 21 hand landmarks
    // roi_image: 224x224 hand ROI image
    // trans_mat_inv: inverse affine transform matrix to restore coordinates to original image
    // landmarks: output 21 landmark coordinates (original image coordinate system)
    // is_left_hand: 是否为左手（左手需要翻转图像）
    // Returns: hand presence confidence (0.0~1.0)
    float detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv, std::vector<cv::Point2f>& landmarks,
                 bool is_left_hand);

    // 兼容旧接口（默认当作右手处理）
    float detect(const cv::Mat& roi_image, const cv::Mat& trans_mat_inv, std::vector<cv::Point2f>& landmarks);

  private:
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter>     interpreter;
    std::vector<char>                        model_buffer;  // for loadFromMemory
    int                                      input_size = 224;

    bool buildInterpreter();
};

#endif
