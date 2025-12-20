#pragma once
#ifndef SIMD_NORMALIZE_H
#define SIMD_NORMALIZE_H

#include <cstddef>
#include <cstdint>

// SIMD 模式枚举
enum class SIMDMode {
    Auto,  // 自动检测最佳实现
    AVX2,  // 强制使用 AVX2
    SSE,   // 强制使用 SSE
    Scalar // 强制使用标量实现
};

namespace SIMDNormalize {

// 初始化 SIMD 检测（程序启动时调用一次）
void Init();

// 设置 SIMD 模式
void SetMode(SIMDMode mode);

// 获取当前 SIMD 模式
SIMDMode GetMode();

// 获取当前实际使用的实现名称
const char* GetCurrentImplementation();

// 检测 CPU 是否支持 AVX2
bool IsAVX2Supported();

// 检测 CPU 是否支持 SSE2
bool IsSSE2Supported();

// 将 uint8 RGB 图像归一化为 float [0, 1]
// src: 源图像数据 (uint8, RGB 交错)
// dst: 目标数据 (float, RGB 交错)
// pixel_count: 像素数量
void NormalizeRGB(const uint8_t* src, float* dst, size_t pixel_count);

// 将单行 uint8 RGB 归一化为 float [0, 1]
// 用于需要逐行处理的情况（如翻转）
void NormalizeRGBRow(const uint8_t* src, float* dst, size_t pixel_count);

// 水平翻转 RGB 图像并归一化为 float [0, 1]
// src: 源图像数据 (uint8, RGB 交错, width * height * 3 字节)
// dst: 目标数据 (float, RGB 交错, width * height * 3 floats)
// width: 图像宽度（像素）
// height: 图像高度（像素）
void FlipHorizontalAndNormalize(const uint8_t* src, float* dst, int width, int height);

// 水平翻转 BGR 图像并转换为 RGB
// src: 源图像数据 (uint8, BGR 交错)
// dst: 目标图像数据 (uint8, RGB 交错)
// width: 图像宽度
// height: 图像高度
void FlipHorizontalAndBGR2RGB(const uint8_t* src, uint8_t* dst, int width, int height);

} // namespace SIMDNormalize

#endif // SIMD_NORMALIZE_H
