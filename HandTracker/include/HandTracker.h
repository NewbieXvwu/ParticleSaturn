#pragma once

#ifdef HANDTRACKER_STATIC
#define HAND_API
#elif defined(HANDTRACKER_EXPORTS)
#define HAND_API __declspec(dllexport)
#else
#define HAND_API __declspec(dllimport)
#endif

// SIMD 模式枚举（与内部 SIMDMode 对应）
enum HandTrackerSIMDMode {
    SIMD_AUTO   = 0, // 自动检测最佳实现
    SIMD_AVX2   = 1, // 强制使用 AVX2
    SIMD_SSE    = 2, // 强制使用 SSE
    SIMD_SCALAR = 3  // 强制使用标量实现
};

// 初始化错误码枚举
enum HandTrackerError {
    HANDTRACKER_OK                  = 0,  // 初始化成功
    HANDTRACKER_ERROR_UNKNOWN       = 1,  // 未知错误
    HANDTRACKER_ERROR_PALM_MODEL    = 2,  // 手掌检测模型加载失败
    HANDTRACKER_ERROR_HAND_MODEL    = 3,  // 手部关键点模型加载失败
    HANDTRACKER_ERROR_CAMERA_OPEN   = 4,  // 摄像头打开失败
    HANDTRACKER_ERROR_CAMERA_IN_USE = 5,  // 摄像头被占用
    HANDTRACKER_ERROR_NO_CAMERA     = 6,  // 未检测到摄像头
    HANDTRACKER_ERROR_THREAD        = 7   // 工作线程创建失败
};

extern "C" {
// 设置嵌入式模型数据（静态链接时在 InitTracker 前调用）
HAND_API void SetEmbeddedModels(const void* palm_data, size_t palm_size, const void* hand_data, size_t hand_size);

// 初始化手部追踪器
// camera_id: 摄像头索引（通常为 0）
// model_dir: 模型目录路径（使用嵌入式模型时可为 nullptr）
HAND_API bool InitTracker(int camera_id, const char* model_dir);

// 获取最后一次错误码
HAND_API int GetTrackerLastError();

// 获取最后一次错误信息（人类可读）
HAND_API const char* GetTrackerLastErrorMessage();

// 获取手部追踪数据（已平滑处理）
// out_scale: 缩放值（拇指-食指距离）
// out_rot_x: X 轴旋转（0.0~1.0）
// out_rot_y: Y 轴旋转（0.0~1.0）
// out_has_hand: 是否检测到手部
HAND_API bool GetHandData(float* out_scale, float* out_rot_x, float* out_rot_y, bool* out_has_hand);

// 释放资源并关闭摄像头
HAND_API void ReleaseTracker();

// 启用/禁用 OpenCV 调试窗口
HAND_API void SetTrackerDebugMode(bool enabled);

// 获取当前调试模式状态
HAND_API bool GetTrackerDebugMode();

// 设置 SIMD 模式 (用于图像归一化)
HAND_API void SetTrackerSIMDMode(int mode);

// 获取当前 SIMD 模式
HAND_API int GetTrackerSIMDMode();

// 获取当前 SIMD 实现名称
HAND_API const char* GetTrackerSIMDImplementation();
}