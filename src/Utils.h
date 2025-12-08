#pragma once
// 工具函数 - 通用辅助函数和数据结构

#include <cmath>

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

inline glm::vec3 HexToRGB(int hex) {
    return glm::vec3(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f);
}
