#include "HandTracker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <opencv2/opencv.hpp>
#include <string>
#include <thread>

#include "HandLandmark.h"
#include "PalmDetector.h"

// One Euro Filter - 低通滤波器，用于平滑手势追踪数据
// 核心特性：静止时强平滑，快速移动时弱平滑以保持响应性
class OneEuroFilter {
  public:
    OneEuroFilter(float minCutoff = 1.0f, float beta = 0.007f, float dCutoff = 1.0f)
        : minCutoff(minCutoff), beta(beta), dCutoff(dCutoff), firstTime(true), prevValue(0), prevDx(0) {}

    float filter(float value, float dt = 1.0f / 30.0f) {
        if (firstTime) {
            firstTime = false;
            prevValue = value;
            prevDx    = 0;
            return value;
        }

        // 计算速度（导数）并平滑
        float dx  = (value - prevValue) / dt;
        float edx = lowPass(dx, prevDx, computeAlpha(dCutoff, dt));
        prevDx    = edx;

        // 根据速度动态调整截止频率
        float cutoff = minCutoff + beta * std::abs(edx);
        float result = lowPass(value, prevValue, computeAlpha(cutoff, dt));
        prevValue    = result;
        return result;
    }

    void reset() {
        firstTime = true;
        prevValue = 0;
        prevDx    = 0;
    }

  private:
    float minCutoff; // 最小截止频率（越小越平滑）
    float beta;      // 速度系数（越大对快速移动响应越快）
    float dCutoff;   // 导数截止频率
    bool  firstTime;
    float prevValue;
    float prevDx;

    float computeAlpha(float cutoff, float dt) {
        float tau = 1.0f / (2.0f * 3.14159265f * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }

    float lowPass(float x, float prev, float a) { return a * x + (1.0f - a) * prev; }
};

static std::atomic<bool> g_debug_mode           = false;
static std::atomic<bool> g_debug_window_created = false;

struct SharedData {
    float scale;
    float rot_x;
    float rot_y;
    bool  has_hand;
};

struct DebugData {
    cv::Mat                  frame;
    std::vector<cv::Point2f> landmarks;
    bool                     has_landmarks = false;
    float                    scale         = 1.0f;
    float                    rot_x         = 0.5f;
    float                    rot_y         = 0.5f;
};

static std::thread*      g_worker_thread = nullptr;
static std::atomic<bool> g_running       = false;
static std::mutex        g_data_mutex;
static std::mutex        g_debug_mutex;
static SharedData        g_latest_data = {1.0f, 0.5f, 0.5f, false};
static DebugData         g_debug_data;

static float g_smooth_scale = 1.0f;
static float g_smooth_rot_x = 0.5f;
static float g_smooth_rot_y = 0.5f;

// One Euro Filter 实例（位置用 0.5/0.5，缩放用 0.2/0.05 以获得更平滑的效果）
static OneEuroFilter g_filter_rot_x(0.5f, 0.5f, 1.0f);
static OneEuroFilter g_filter_rot_y(0.5f, 0.5f, 1.0f);
static OneEuroFilter g_filter_scale(0.2f, 0.05f, 1.0f);

const int HAND_LOST_FRAMES = 10;

static const void* g_palm_model_data = nullptr;
static size_t      g_palm_model_size = 0;
static const void* g_hand_model_data = nullptr;
static size_t      g_hand_model_size = 0;

static std::string JoinPath(const std::string& folder, const std::string& filename) {
    if (folder.empty()) {
        return filename;
    }
    char last = folder.back();
    if (last == '/' || last == '\\') {
        return folder + filename;
    }
    return folder + "/" + filename;
}

void WorkerThreadFunc(int cam_id, std::string model_dir) {
    PalmDetector palm_detector;
    HandLandmark landmark_detector;

    bool palm_loaded     = false;
    bool landmark_loaded = false;

    if (g_palm_model_data && g_palm_model_size > 0) {
        palm_loaded     = palm_detector.loadFromMemory(g_palm_model_data, g_palm_model_size);
        landmark_loaded = landmark_detector.loadFromMemory(g_hand_model_data, g_hand_model_size);
    } else {
        std::string palm_path     = JoinPath(model_dir, "palm_detection_full.tflite");
        std::string landmark_path = JoinPath(model_dir, "hand_landmark_full.tflite");
        palm_loaded               = palm_detector.load(palm_path);
        landmark_loaded           = landmark_detector.load(landmark_path);
    }

    if (!palm_loaded) {
        std::cerr << "[HandTracker] Error: Failed to load palm detection model" << std::endl;
        g_running = false;
        return;
    }

    if (!landmark_loaded) {
        std::cerr << "[HandTracker] Error: Failed to load hand landmark model" << std::endl;
        g_running = false;
        return;
    }

    cv::VideoCapture cap;
    bool             camera_ok = false;

    try {
        cap.open(cam_id, cv::CAP_DSHOW);
    } catch (...) {}
    if (cap.isOpened()) {
        camera_ok = true;
    } else {
        try {
            cap.open(cam_id, cv::CAP_MSMF);
        } catch (...) {}
        if (cap.isOpened()) {
            camera_ok = true;
        } else {
            try {
                cap.open(cam_id, cv::CAP_ANY);
            } catch (...) {}
            if (cap.isOpened()) {
                camera_ok = true;
            }
        }
    }

    if (!camera_ok) {
        std::cerr << "[HandTracker] Error: Failed to open camera " << cam_id << std::endl;
        g_running = false;
        return;
    }

    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    std::cout << "[HandTracker] Camera opened, starting detection loop..." << std::endl;

    cv::Mat                  frame;
    int                      frame_count = 0;
    std::vector<cv::Point2f> debug_landmarks;
    bool                     debug_landmarks_valid = false;
    int                      hand_lost_counter     = 0;
    bool                     smooth_has_hand       = false;

    while (g_running) {
        if (!cap.read(frame)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        frame_count++;
        debug_landmarks_valid = false;

        cv::flip(frame, frame, 1);

        cv::Mat frame_rgb;
        cv::cvtColor(frame, frame_rgb, cv::COLOR_BGR2RGB);

        std::vector<PalmDetection> palms = palm_detector.detect(frame_rgb, 0.4f, 0.3f);

        float target_scale     = 1.0f;
        float target_rot_x     = 0.5f;
        float target_rot_y     = 0.5f;
        bool  current_has_hand = false;

        bool is_left_hand = false;

        if (!palms.empty()) {
            current_has_hand          = true;
            const PalmDetection& palm = palms[0];

            // 提取手部 ROI 并进行仿射变换
            int img_w = frame.cols;
            int img_h = frame.rows;

            cv::Point2f srcPts[3], dstPts[3];
            srcPts[0] = cv::Point2f(palm.hand_pos[0].x * img_w, palm.hand_pos[0].y * img_h);
            srcPts[1] = cv::Point2f(palm.hand_pos[1].x * img_w, palm.hand_pos[1].y * img_h);
            srcPts[2] = cv::Point2f(palm.hand_pos[2].x * img_w, palm.hand_pos[2].y * img_h);

            dstPts[0] = cv::Point2f(0, 0);
            dstPts[1] = cv::Point2f(224, 0);
            dstPts[2] = cv::Point2f(224, 224);

            cv::Mat trans_mat = cv::getAffineTransform(srcPts, dstPts);
            cv::Mat roi_image;
            cv::warpAffine(frame_rgb, roi_image, trans_mat, cv::Size(224, 224));

            cv::Mat trans_mat_inv;
            cv::invertAffineTransform(trans_mat, trans_mat_inv);

            // 通过手掌关键点判断左右手
            // palm.landmarks: 0=手腕, 2=中指根部, 其他点用于辅助判断
            // 在镜像后的图像中：
            // - 如果拇指在中指的右边（x更大），则是左手（用户的右手镜像后）
            // - 如果拇指在中指的左边（x更小），则是右手（用户的左手镜像后）
            // 使用关键点 0(手腕), 2(中指根部) 和手掌方向来判断
            float wrist_x  = palm.landmarks[0].x;
            float middle_x = palm.landmarks[2].x;
            float wrist_y  = palm.landmarks[0].y;
            float middle_y = palm.landmarks[2].y;

            // 计算手掌朝向向量（从手腕到中指根部）
            float palm_dir_x = middle_x - wrist_x;
            float palm_dir_y = middle_y - wrist_y;

            // 使用叉积判断：如果拇指在手掌方向的右侧，则是左手
            // 这里用关键点1（拇指侧）和关键点5（小指侧）的相对位置
            // 或者简单地用关键点的x坐标差异
            float thumb_side_x = palm.landmarks[1].x; // 拇指侧关键点
            float pinky_side_x = palm.landmarks[5].x; // 小指侧关键点（如果有的话）

            // 简化判断：在镜像图像中，如果拇指侧在中指的右边，是左手
            // 计算拇指侧相对于手腕-中指连线的位置
            float cross  = palm_dir_x * (palm.landmarks[1].y - wrist_y) - palm_dir_y * (thumb_side_x - wrist_x);
            is_left_hand = (cross > 0); // 叉积为正表示拇指在左侧（镜像后的左手）

            // 检测手部关键点，传入左右手信息
            std::vector<cv::Point2f> landmarks;
            float presence = landmark_detector.detect(roi_image, trans_mat_inv, landmarks, is_left_hand);

            bool landmarks_valid = (landmarks.size() >= 21 && presence > 0.1f);

            debug_landmarks_valid = landmarks_valid;
            if (landmarks_valid) {
                debug_landmarks = landmarks;

                // 计算拇指和食指距离作为缩放值
                float thumb_x = landmarks[4].x / img_w;
                float thumb_y = landmarks[4].y / img_h;
                float index_x = landmarks[8].x / img_w;
                float index_y = landmarks[8].y / img_h;

                float dist     = std::hypot(thumb_x - index_x, thumb_y - index_y);
                float normDist = std::max(0.0f, std::min(1.0f, (dist - 0.02f) / 0.25f));
                target_scale   = 0.5f + normDist * 2.0f;

                // 使用手腕位置作为旋转控制
                float wrist_x = landmarks[0].x / img_w;
                float wrist_y = landmarks[0].y / img_h;

                target_rot_x = std::max(0.0f, std::min(1.0f, wrist_x));
                target_rot_y = std::max(0.0f, std::min(1.0f, wrist_y));
            } else {
                // 关键点检测失败时使用手掌中心位置
                float pos_x  = std::max(0.0f, std::min(1.0f, palm.hand_cx));
                float pos_y  = std::max(0.0f, std::min(1.0f, palm.hand_cy));
                target_rot_x = pos_x;
                target_rot_y = pos_y;

                float hand_size = std::max(palm.hand_w, palm.hand_h);
                float normSize  = std::max(0.0f, std::min(1.0f, (hand_size - 0.3f) / 0.5f));
                target_scale    = 0.5f + normSize * 2.0f;
            }
        }

        // 使用 One Euro Filter 平滑数据
        const float dt = 1.0f / 30.0f;
        g_smooth_rot_x = g_filter_rot_x.filter(target_rot_x, dt);
        g_smooth_rot_y = g_filter_rot_y.filter(target_rot_y, dt);
        g_smooth_scale = g_filter_scale.filter(target_scale, dt);

        // 手部丢失延迟处理（避免闪烁）
        if (current_has_hand) {
            hand_lost_counter = 0;
            smooth_has_hand   = true;
        } else {
            hand_lost_counter++;
            if (hand_lost_counter >= HAND_LOST_FRAMES) {
                smooth_has_hand = false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_data_mutex);
            g_latest_data.has_hand = smooth_has_hand;
            g_latest_data.scale    = g_smooth_scale;
            g_latest_data.rot_x    = g_smooth_rot_x;
            g_latest_data.rot_y    = g_smooth_rot_y;
        }

        // 调试窗口显示 (优化: 只在调试模式下才 clone frame)
        if (g_debug_mode) {
            if (!g_debug_window_created) {
                cv::namedWindow("HandTracker Debug", cv::WINDOW_AUTOSIZE);
                g_debug_window_created = true;
            }

            cv::Mat debug_frame;
            frame.copyTo(debug_frame);  // 只在调试模式下复制
            int     img_w       = frame.cols;
            int     img_h       = frame.rows;

            if (current_has_hand && !palms.empty()) {
                const PalmDetection& palm = palms[0];

                // 绘制手掌检测框
                cv::Rect2f rect = palm.rect;
                cv::rectangle(debug_frame, cv::Point((int)(rect.x * img_w), (int)(rect.y * img_h)),
                              cv::Point((int)((rect.x + rect.width) * img_w), (int)((rect.y + rect.height) * img_h)),
                              cv::Scalar(0, 255, 255), 2);

                // 绘制手部 ROI 四边形
                for (int i = 0; i < 4; i++) {
                    cv::line(debug_frame,
                             cv::Point((int)(palm.hand_pos[i].x * img_w), (int)(palm.hand_pos[i].y * img_h)),
                             cv::Point((int)(palm.hand_pos[(i + 1) % 4].x * img_w),
                                       (int)(palm.hand_pos[(i + 1) % 4].y * img_h)),
                             cv::Scalar(255, 0, 255), 2);
                }

                // 绘制手部关键点和连接线
                if (debug_landmarks_valid && debug_landmarks.size() >= 21) {
                    const auto&      lm               = debug_landmarks;
                    static const int connections[][2] = {{0, 1},   {1, 2},   {2, 3},   {3, 4},   {0, 5},   {5, 6},
                                                         {6, 7},   {7, 8},   {0, 9},   {9, 10},  {10, 11}, {11, 12},
                                                         {0, 13},  {13, 14}, {14, 15}, {15, 16}, {0, 17},  {17, 18},
                                                         {18, 19}, {19, 20}, {5, 9},   {9, 13},  {13, 17}};

                    // 绘制骨架连接线
                    for (auto& conn : connections) {
                        cv::line(debug_frame, cv::Point((int)lm[conn[0]].x, (int)lm[conn[0]].y),
                                 cv::Point((int)lm[conn[1]].x, (int)lm[conn[1]].y), cv::Scalar(0, 255, 0), 2);
                    }

                    // 绘制关键点
                    for (int i = 0; i < 21; i++) {
                        cv::Scalar color;
                        if (i == 0) {
                            color = cv::Scalar(255, 0, 0);
                        } else if (i == 4) {
                            color = cv::Scalar(0, 0, 255);
                        } else if (i == 8) {
                            color = cv::Scalar(0, 0, 255);
                        } else {
                            color = cv::Scalar(0, 255, 255);
                        }

                        cv::circle(debug_frame, cv::Point((int)lm[i].x, (int)lm[i].y),
                                   (i == 0 || i == 4 || i == 8) ? 8 : 5, color, -1);
                    }

                    cv::line(debug_frame, cv::Point((int)lm[4].x, (int)lm[4].y), cv::Point((int)lm[8].x, (int)lm[8].y),
                             cv::Scalar(0, 0, 255), 3);
                }
            }

            char info[256];
            snprintf(info, sizeof(info), "Scale: %.2f  RotX: %.2f  RotY: %.2f", g_smooth_scale, g_smooth_rot_x,
                     g_smooth_rot_y);
            cv::putText(debug_frame, info, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(0, 255, 0), 2);

            char hand_info[128];
            snprintf(hand_info, sizeof(hand_info), "Hand: %s (raw: %s, lost: %d)", smooth_has_hand ? "YES" : "NO",
                     current_has_hand ? "Y" : "N", hand_lost_counter);
            cv::putText(debug_frame, hand_info, cv::Point(10, 60), cv::FONT_HERSHEY_SIMPLEX, 0.5,
                        smooth_has_hand ? cv::Scalar(0, 255, 0) : cv::Scalar(0, 0, 255), 2);

            cv::imshow("HandTracker Debug", debug_frame);
            cv::waitKey(1);
        } else if (g_debug_window_created) {
            cv::destroyWindow("HandTracker Debug");
            g_debug_window_created = false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (g_debug_window_created) {
        cv::destroyWindow("HandTracker Debug");
        g_debug_window_created = false;
    }
    cap.release();
    std::cout << "[HandTracker] Worker thread stopped" << std::endl;
}

HAND_API void SetEmbeddedModels(const void* palm_data, size_t palm_size, const void* hand_data, size_t hand_size) {
    g_palm_model_data = palm_data;
    g_palm_model_size = palm_size;
    g_hand_model_data = hand_data;
    g_hand_model_size = hand_size;
}

HAND_API bool InitTracker(int camera_id, const char* model_dir) {
    if (g_running) {
        return true;
    }

    if (model_dir == nullptr && g_palm_model_data == nullptr) {
        return false;
    }

    g_running = true;
    try {
        g_worker_thread =
            new std::thread(WorkerThreadFunc, camera_id, model_dir ? std::string(model_dir) : std::string());
    } catch (...) {
        g_running = false;
        if (g_worker_thread) {
            delete g_worker_thread;
            g_worker_thread = nullptr;
        }
        return false;
    }
    return true;
}

HAND_API bool GetHandData(float* out_scale, float* out_rot_x, float* out_rot_y, bool* out_has_hand) {
    std::lock_guard<std::mutex> lock(g_data_mutex);

    if (out_scale) {
        *out_scale = g_latest_data.scale;
    }
    if (out_rot_x) {
        *out_rot_x = g_latest_data.rot_x;
    }
    if (out_rot_y) {
        *out_rot_y = g_latest_data.rot_y;
    }
    if (out_has_hand) {
        *out_has_hand = g_latest_data.has_hand;
    }

    return g_latest_data.has_hand;
}

HAND_API void ReleaseTracker() {
    g_running = false;
    if (g_worker_thread) {
        if (g_worker_thread->joinable()) {
            g_worker_thread->join();
        }
        delete g_worker_thread;
        g_worker_thread = nullptr;
    }

    g_smooth_scale = 1.0f;
    g_smooth_rot_x = 0.5f;
    g_smooth_rot_y = 0.5f;
    g_latest_data  = {1.0f, 0.5f, 0.5f, false};

    // 重置滤波器
    g_filter_rot_x.reset();
    g_filter_rot_y.reset();
    g_filter_scale.reset();
}

HAND_API void SetTrackerDebugMode(bool enabled) {
    g_debug_mode = enabled;
}

HAND_API bool GetTrackerDebugMode() {
    return g_debug_mode;
}
