#pragma once
// 工具函数 - 通用辅助函数和数据结构

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

// 前向声明 HandTracker API (避免循环依赖)
// 完整声明在 HandTracker.h 中
extern "C" {
#ifdef HANDTRACKER_STATIC
#define HAND_API_FWD
#elif defined(HANDTRACKER_EXPORTS)
#define HAND_API_FWD __declspec(dllexport)
#else
#define HAND_API_FWD __declspec(dllimport)
#endif
HAND_API_FWD bool GetHandData(float* out_scale, float* out_rot_x, float* out_rot_y, bool* out_has_hand);
}

// 动画辅助类
struct AnimFloat {
    float val    = 0.0f;
    float target = 0.0f;

    void Update(float dt, float speed = 15.0f) {
        val += (target - val) * (1.0f - std::exp(-speed * dt));
        if (std::abs(target - val) < 0.001f) {
            val = target;
        }
    }
};

// 平滑动画状态
struct SmoothState {
    float scale = 1.0f;
    float rotX  = 0.4f;
    float rotY  = 0.0f;
};

// 手部追踪状态
struct HandState {
    bool  hasHand = false;
    float scale   = 1.0f;
    float rotX    = 0.5f;
    float rotY    = 0.5f;
};

// 工具函数
inline float Lerp(float a, float b, float f) {
    return a + f * (b - a);
}

// 缓动函数：ease-out cubic (快速开始，缓慢结束)
inline float EaseOutCubic(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); // clamp
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

// Catmull-Rom 样条插值
// 给定4个控制点 p0, p1, p2, p3，计算 p1 到 p2 之间 t 位置的值
// t ∈ [0, 1]，t=0 时返回 p1，t=1 时返回 p2
inline float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

inline glm::vec3 HexToRGB(int hex) {
    return glm::vec3(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}

// 环形缓冲区 FPS 计算器 (优化: 提供更平滑的 FPS 统计)
// 使用固定大小的环形缓冲区存储最近 N 帧的帧时间，计算滑动平均
template <int N = 60> class RingBufferFPS {
  public:
    RingBufferFPS() {
        // 预填充假设 60 FPS 的帧时间，避免启动时 FPS 从 0 涨上去
        for (int i = 0; i < N; i++) {
            frameTimes[i] = 1.0f / 60.0f;
        }
        // 初始化显示用历史（60个采样点，每个代表约50ms）
        for (int i = 0; i < DISPLAY_HISTORY_SIZE; i++) {
            displayHistory[i] = 60.0f;
        }
        count = N; // 标记为已填满，与 sum 的初始值匹配
    }

    // 添加新的帧时间
    void AddFrameTime(float dt) {
        sum -= frameTimes[index];
        frameTimes[index] = dt;
        sum += dt;
        index = (index + 1) % N;

        // 累积时间用于低频采样显示历史
        historyAccumTime += dt;
        historyAccumFps += (dt > 0.0f) ? 1.0f / dt : 60.0f;
        historyAccumCount++;

        // 每 50ms 更新一次显示历史（约 20Hz）
        if (historyAccumTime >= 0.05f) {
            float avgFps                        = historyAccumFps / historyAccumCount;
            displayHistory[displayHistoryIndex] = avgFps;
            displayHistoryIndex                 = (displayHistoryIndex + 1) % DISPLAY_HISTORY_SIZE;
            // 保留超出部分的时间，确保精确的更新频率
            historyAccumTime -= 0.05f;
            historyAccumFps   = 0.0f;
            historyAccumCount = 0;
            // 重置滚动动画（保留本帧的进度）
            scrollAnimTime = historyAccumTime;
        } else {
            // 仅在非更新帧累加动画时间
            scrollAnimTime += dt;
        }
    }

    // 获取平均 FPS
    float GetAverageFPS() const {
        if (sum <= 0.0f) {
            return 60.0f;
        }
        return (float)N / sum; // 始终使用 N 作为分子
    }

    // 获取平均帧时间
    float GetAverageFrameTime() const {
        if (sum <= 0.0f) {
            return 1.0f / 60.0f;
        }
        return sum / (float)N; // 始终使用 N 作为分母
    }

    // 获取显示用 FPS 历史（低频采样，用于曲线图）
    const float* GetDisplayHistory() const { return displayHistory; }

    int GetDisplayHistorySize() const { return DISPLAY_HISTORY_SIZE; }

    int GetDisplayHistoryIndex() const { return displayHistoryIndex; }

    // 获取滚动动画进度 (0 = 刚更新，1 = 即将更新)
    // 使用 ease-out 缓动，让动画开始快结束慢
    float GetScrollProgress() const {
        // 50ms 更新周期
        float t = scrollAnimTime / 0.05f;
        return EaseOutCubic(t < 1.0f ? t : 1.0f);
    }

    // 获取更新周期（秒）
    static constexpr float GetUpdateInterval() { return 0.05f; }

  private:
    float frameTimes[N];
    float sum   = N * (1.0f / 60.0f); // 初始假设 60 FPS
    int   index = 0;
    int   count = N; // 初始化为 N，与预填充的数据匹配

    // 显示用历史（低频采样）
    static const int DISPLAY_HISTORY_SIZE = 60;
    float            displayHistory[DISPLAY_HISTORY_SIZE];
    int              displayHistoryIndex = 0;
    float            historyAccumTime    = 0.0f;
    float            historyAccumFps     = 0.0f;
    int              historyAccumCount   = 0;

    // 滚动动画状态
    float scrollAnimTime = 0.0f;
};

// 异步手部追踪器 (优化: 将手部追踪从主线程解耦，消除阻塞)
// 后台线程持续更新手部数据，主循环只需读取最新状态
class AsyncHandTracker {
  public:
    void Start() {
        if (running.load()) {
            return;
        }
        running.store(true);
        trackerThread = std::thread(&AsyncHandTracker::TrackingLoop, this);
    }

    void Stop() {
        running.store(false);
        if (trackerThread.joinable()) {
            trackerThread.join();
        }
    }

    HandState GetLatestState() {
        std::lock_guard<std::mutex> lock(stateMutex);
        return latestState;
    }

    ~AsyncHandTracker() { Stop(); }

  private:
    void TrackingLoop() {
        while (running.load()) {
            HandState temp;
            GetHandData(&temp.scale, &temp.rotX, &temp.rotY, &temp.hasHand);
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                latestState = temp;
            }
            // 约60 FPS的追踪频率，足够流畅且不过度占用CPU
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    std::thread       trackerThread;
    std::atomic<bool> running{false};
    std::mutex        stateMutex;
    HandState         latestState;
};
