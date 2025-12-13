#pragma once
#ifndef PALM_DETECTOR_H
#define PALM_DETECTOR_H

#include <memory>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

#include "tensorflow/lite/interpreter.h"
#include "tensorflow/lite/model.h"

// Palm detection result
struct PalmDetection {
    float       score;        // confidence
    cv::Rect2f  rect;         // palm bounding box (normalized 0~1)
    cv::Point2f landmarks[7]; // 7 keypoints (normalized)
    float       rotation;     // rotation angle

    // Extended hand ROI info (for subsequent landmark detection)
    float       hand_cx, hand_cy; // hand center (normalized)
    float       hand_w, hand_h;   // hand region size (normalized)
    cv::Point2f hand_pos[4];      // ROI four corners (normalized)
};

// Palm detector - based on MediaPipe Palm Detection model
class PalmDetector {
  public:
    PalmDetector();
    ~PalmDetector();

    bool load(const std::string& model_path);
    bool loadFromMemory(const void* data, size_t size);

    // Detect palms in image
    // image: input image (BGR or RGB)
    // prob_threshold: confidence threshold
    // nms_threshold: NMS threshold
    std::vector<PalmDetection> detect(const cv::Mat& image, float prob_threshold = 0.5f, float nms_threshold = 0.3f);

  private:
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter>     interpreter;
    std::vector<char>                        model_buffer;
    int                                      input_size = 192;

    struct Anchor {
        float x_center, y_center;
    };

    std::vector<Anchor> anchors;

    bool buildInterpreter();
    void generateAnchors();
    void decodeDetections(const float* scores, const float* boxes, int num_anchors,
                          std::vector<PalmDetection>& detections, float threshold);
    void computeRotation(PalmDetection& det);
    void convertToHandROI(PalmDetection& det);
};

#endif
