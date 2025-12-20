#pragma once
// 工具函数 - 通用辅助函数和数据结构

#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

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

// 行星属性 (预定义以减少每帧计算)
struct PlanetData {
    glm::vec3 pos;      // 位置
    float     radius;   // 半径
    glm::vec3 color1;   // 颜色1
    glm::vec3 color2;   // 颜色2
    float     noiseScale;  // 噪声缩放
    float     atmosphere;  // 大气层强度
};

// 行星实例数据 (用于 UBO 实例化渲染, 符合 std140 布局)
struct PlanetInstance {
    glm::mat4 modelMatrix;  // 64 字节
    glm::vec4 color1;       // 16 字节 (xyz = color, w = noiseScale)
    glm::vec4 color2;       // 16 字节 (xyz = color, w = atmosphere)
    // 总计 96 字节每实例
};

// 工具函数
inline float Lerp(float a, float b, float f) {
    return a + f * (b - a);
}

inline glm::vec3 HexToRGB(int hex) {
    return glm::vec3(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}

// 环形缓冲区 FPS 计算器 (优化: 提供更平滑的 FPS 统计)
// 使用固定大小的环形缓冲区存储最近 N 帧的帧时间，计算滑动平均
template<int N = 60>
class RingBufferFPS {
public:
    RingBufferFPS() {
        for (int i = 0; i < N; i++) frameTimes[i] = 1.0f / 60.0f;
    }

    // 添加新的帧时间
    void AddFrameTime(float dt) {
        sum -= frameTimes[index];
        frameTimes[index] = dt;
        sum += dt;
        index = (index + 1) % N;
        if (count < N) count++;
    }

    // 获取平均 FPS
    float GetAverageFPS() const {
        if (count == 0 || sum <= 0.0f) return 60.0f;
        return (float)count / sum;
    }

    // 获取平均帧时间
    float GetAverageFrameTime() const {
        if (count == 0) return 1.0f / 60.0f;
        return sum / (float)count;
    }

private:
    float frameTimes[N];
    float sum = N * (1.0f / 60.0f);  // 初始假设 60 FPS
    int   index = 0;
    int   count = 0;
};

// 异步手部追踪器 (优化: 将手部追踪从主线程解耦，消除阻塞)
// 后台线程持续更新手部数据，主循环只需读取最新状态
class AsyncHandTracker {
public:
    void Start() {
        if (running.load()) return;
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

    ~AsyncHandTracker() {
        Stop();
    }

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

    std::thread trackerThread;
    std::atomic<bool> running{false};
    std::mutex stateMutex;
    HandState latestState;
};
