// SIMDNormalize.cpp - SIMD 加速的图像归一化
// 支持 AVX2、SSE2 和标量回退

#include "SIMDNormalize.h"

#include <iostream>

#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

// SIMD 内联函数
#include <immintrin.h>  // AVX2, AVX, SSE

namespace SIMDNormalize {

// CPU 特性检测结果（缓存）
static bool g_initialized = false;
static bool g_hasAVX2 = false;
static bool g_hasSSE2 = false;
static SIMDMode g_currentMode = SIMDMode::Auto;

// CPUID 辅助函数
static void GetCPUID(int info[4], int function_id) {
#ifdef _MSC_VER
    __cpuid(info, function_id);
#else
    __cpuid(function_id, info[0], info[1], info[2], info[3]);
#endif
}

static void GetCPUIDEx(int info[4], int function_id, int subfunction_id) {
#ifdef _MSC_VER
    __cpuidex(info, function_id, subfunction_id);
#else
    __cpuid_count(function_id, subfunction_id, info[0], info[1], info[2], info[3]);
#endif
}

void Init() {
    if (g_initialized) return;

    int info[4];

    // 检查 CPUID 支持
    GetCPUID(info, 0);
    int maxFunction = info[0];

    if (maxFunction >= 1) {
        GetCPUID(info, 1);
        // SSE2: EDX bit 26
        g_hasSSE2 = (info[3] & (1 << 26)) != 0;
    }

    if (maxFunction >= 7) {
        GetCPUIDEx(info, 7, 0);
        // AVX2: EBX bit 5
        g_hasAVX2 = (info[1] & (1 << 5)) != 0;
    }

    g_initialized = true;

    std::cout << "[SIMD] CPU features detected - AVX2: " << (g_hasAVX2 ? "Yes" : "No")
              << ", SSE2: " << (g_hasSSE2 ? "Yes" : "No") << std::endl;
}

void SetMode(SIMDMode mode) {
    g_currentMode = mode;
    std::cout << "[SIMD] Mode set to: " << GetCurrentImplementation() << std::endl;
}

SIMDMode GetMode() {
    return g_currentMode;
}

bool IsAVX2Supported() {
    if (!g_initialized) Init();
    return g_hasAVX2;
}

bool IsSSE2Supported() {
    if (!g_initialized) Init();
    return g_hasSSE2;
}

const char* GetCurrentImplementation() {
    if (!g_initialized) Init();

    switch (g_currentMode) {
        case SIMDMode::AVX2:
            return g_hasAVX2 ? "AVX2 (forced)" : "AVX2 (unavailable, using fallback)";
        case SIMDMode::SSE:
            return g_hasSSE2 ? "SSE (forced)" : "SSE (unavailable, using scalar)";
        case SIMDMode::Scalar:
            return "Scalar (forced)";
        case SIMDMode::Auto:
        default:
            if (g_hasAVX2) return "AVX2 (auto)";
            if (g_hasSSE2) return "SSE (auto)";
            return "Scalar (auto)";
    }
}

// ============================================================================
// 标量实现
// ============================================================================
static void NormalizeRGB_Scalar(const uint8_t* src, float* dst, size_t pixel_count) {
    const float scale = 1.0f / 255.0f;
    size_t total = pixel_count * 3;
    for (size_t i = 0; i < total; ++i) {
        dst[i] = src[i] * scale;
    }
}

// ============================================================================
// SSE 实现 (一次处理 4 个像素 = 12 个通道)
// ============================================================================
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
static void NormalizeRGB_SSE(const uint8_t* src, float* dst, size_t pixel_count) {
    const __m128 scale = _mm_set1_ps(1.0f / 255.0f);
    const __m128i zero = _mm_setzero_si128();

    size_t i = 0;
    // 每次处理 4 个像素 (12 bytes -> 12 floats)
    // 但 SSE 寄存器是 128 位 (4 floats)，所以需要 3 次存储
    size_t simd_count = (pixel_count / 4) * 4;

    for (; i < simd_count; i += 4) {
        // 加载 12 字节 (4 像素的 RGB) - 使用 16 字节加载，忽略最后 4 字节
        // 注意：这里可能读取超出边界，但后面的标量处理会覆盖正确值
        __m128i pixels = _mm_loadu_si128((const __m128i*)(src + i * 3));

        // 解包 uint8 -> int16 -> int32
        __m128i lo16 = _mm_unpacklo_epi8(pixels, zero);  // 前 8 个 uint8 -> 8 个 int16

        // int16 -> int32 (前 4 个)
        __m128i lo32_0 = _mm_unpacklo_epi16(lo16, zero);  // 4 个 int32
        __m128i lo32_1 = _mm_unpackhi_epi16(lo16, zero);  // 4 个 int32

        // 再解包一次获取第三组
        __m128i hi16 = _mm_unpackhi_epi8(pixels, zero);
        __m128i lo32_2 = _mm_unpacklo_epi16(hi16, zero);

        // int32 -> float 并乘以 scale
        __m128 f0 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_0), scale);
        __m128 f1 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_1), scale);
        __m128 f2 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_2), scale);

        // 存储 12 个 float
        _mm_storeu_ps(dst + i * 3 + 0, f0);
        _mm_storeu_ps(dst + i * 3 + 4, f1);
        _mm_storeu_ps(dst + i * 3 + 8, f2);
    }

    // 处理剩余像素
    const float scalar_scale = 1.0f / 255.0f;
    for (; i < pixel_count; ++i) {
        dst[i * 3 + 0] = src[i * 3 + 0] * scalar_scale;
        dst[i * 3 + 1] = src[i * 3 + 1] * scalar_scale;
        dst[i * 3 + 2] = src[i * 3 + 2] * scalar_scale;
    }
}
#endif

// ============================================================================
// AVX2 实现 (一次处理 8 个像素 = 24 个通道)
// ============================================================================
#if defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__))
// 注意：MSVC 需要 /arch:AVX2 编译选项
#define HAS_AVX2_IMPL 1
#else
// 对于 MSVC，我们通过运行时检测来使用 AVX2
#ifdef _MSC_VER
#define HAS_AVX2_IMPL 1
#endif
#endif

#ifdef HAS_AVX2_IMPL
__declspec(noinline) static void NormalizeRGB_AVX2(const uint8_t* src, float* dst, size_t pixel_count) {
    const __m256 scale = _mm256_set1_ps(1.0f / 255.0f);

    size_t i = 0;
    // 每次处理 8 个像素 (24 bytes -> 24 floats)
    size_t simd_count = (pixel_count / 8) * 8;

    for (; i < simd_count; i += 8) {
        // 加载 24 字节 - 使用两次 128 位加载
        __m128i pixels_lo = _mm_loadu_si128((const __m128i*)(src + i * 3));       // 前 16 字节
        __m128i pixels_hi = _mm_loadl_epi64((const __m128i*)(src + i * 3 + 16));  // 后 8 字节

        // 组合成 256 位
        __m256i pixels = _mm256_set_m128i(pixels_hi, pixels_lo);

        // 使用 AVX2 的 _mm256_cvtepu8_epi32 一次将 8 个 uint8 转为 8 个 int32
        // 但我们有 24 个通道，需要分 3 组处理

        // 提取前 8 个字节 -> 8 个 int32
        __m256i i32_0 = _mm256_cvtepu8_epi32(pixels_lo);

        // 提取中间 8 个字节
        __m128i mid8 = _mm_srli_si128(pixels_lo, 8);  // 移位获取第 8-15 字节
        __m256i i32_1 = _mm256_cvtepu8_epi32(mid8);

        // 提取后 8 个字节
        __m256i i32_2 = _mm256_cvtepu8_epi32(pixels_hi);

        // int32 -> float 并乘以 scale
        __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_0), scale);
        __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_1), scale);
        __m256 f2 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_2), scale);

        // 存储 24 个 float
        _mm256_storeu_ps(dst + i * 3 + 0, f0);
        _mm256_storeu_ps(dst + i * 3 + 8, f1);
        _mm256_storeu_ps(dst + i * 3 + 16, f2);
    }

    // 处理剩余像素 (使用标量)
    const float scalar_scale = 1.0f / 255.0f;
    for (; i < pixel_count; ++i) {
        dst[i * 3 + 0] = src[i * 3 + 0] * scalar_scale;
        dst[i * 3 + 1] = src[i * 3 + 1] * scalar_scale;
        dst[i * 3 + 2] = src[i * 3 + 2] * scalar_scale;
    }
}
#endif

// ============================================================================
// 统一接口
// ============================================================================
void NormalizeRGB(const uint8_t* src, float* dst, size_t pixel_count) {
    if (!g_initialized) Init();

    // 根据模式选择实现
    SIMDMode effectiveMode = g_currentMode;

    if (effectiveMode == SIMDMode::Auto) {
        // 自动选择最佳实现
#ifdef HAS_AVX2_IMPL
        if (g_hasAVX2) {
            NormalizeRGB_AVX2(src, dst, pixel_count);
            return;
        }
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (g_hasSSE2) {
            NormalizeRGB_SSE(src, dst, pixel_count);
            return;
        }
#endif
        NormalizeRGB_Scalar(src, dst, pixel_count);
        return;
    }

    // 强制指定的模式
    switch (effectiveMode) {
        case SIMDMode::AVX2:
#ifdef HAS_AVX2_IMPL
            if (g_hasAVX2) {
                NormalizeRGB_AVX2(src, dst, pixel_count);
                return;
            }
#endif
            // 回退
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
            if (g_hasSSE2) {
                NormalizeRGB_SSE(src, dst, pixel_count);
                return;
            }
#endif
            NormalizeRGB_Scalar(src, dst, pixel_count);
            break;

        case SIMDMode::SSE:
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
            if (g_hasSSE2) {
                NormalizeRGB_SSE(src, dst, pixel_count);
                return;
            }
#endif
            NormalizeRGB_Scalar(src, dst, pixel_count);
            break;

        case SIMDMode::Scalar:
        default:
            NormalizeRGB_Scalar(src, dst, pixel_count);
            break;
    }
}

// 行级别归一化（用于逐行处理的情况）
void NormalizeRGBRow(const uint8_t* src, float* dst, size_t pixel_count) {
    // 直接调用主函数，因为逻辑相同
    NormalizeRGB(src, dst, pixel_count);
}

// ============================================================================
// 水平翻转并归一化 - 标量实现
// ============================================================================
static void FlipHorizontalAndNormalize_Scalar(const uint8_t* src, float* dst, int width, int height) {
    const float scale = 1.0f / 255.0f;
    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        float* dst_row = dst + y * width * 3;
        for (int x = 0; x < width; ++x) {
            int src_x = (width - 1 - x) * 3;
            int dst_x = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 0] * scale;
            dst_row[dst_x + 1] = src_row[src_x + 1] * scale;
            dst_row[dst_x + 2] = src_row[src_x + 2] * scale;
        }
    }
}

// ============================================================================
// 水平翻转并归一化 - SSE 实现
// ============================================================================
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
static void FlipHorizontalAndNormalize_SSE(const uint8_t* src, float* dst, int width, int height) {
    const __m128 scale = _mm_set1_ps(1.0f / 255.0f);
    const __m128i zero = _mm_setzero_si128();
    const float scalar_scale = 1.0f / 255.0f;

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        float* dst_row = dst + y * width * 3;

        int x = 0;
        // SSE: 每次处理 4 个像素
        int simd_end = width - 4;

        for (; x <= simd_end; x += 4) {
            // 从源图像右侧读取 4 个像素 (翻转后的位置)
            // 源像素位置: (width-1-x), (width-2-x), (width-3-x), (width-4-x)
            int src_x = (width - 1 - x) * 3;

            // 加载 4 个像素 (12 字节)，但需要反向排列
            // 像素顺序: P3, P2, P1, P0 -> 需要变成 P0, P1, P2, P3
            uint8_t pixels[16];
            // 手动反向加载像素
            for (int i = 0; i < 4; ++i) {
                int src_px = src_x - i * 3;
                pixels[i * 3 + 0] = src_row[src_px + 0];
                pixels[i * 3 + 1] = src_row[src_px + 1];
                pixels[i * 3 + 2] = src_row[src_px + 2];
            }

            __m128i px = _mm_loadu_si128((const __m128i*)pixels);

            // 解包 uint8 -> int16 -> int32 -> float
            __m128i lo16 = _mm_unpacklo_epi8(px, zero);
            __m128i lo32_0 = _mm_unpacklo_epi16(lo16, zero);
            __m128i lo32_1 = _mm_unpackhi_epi16(lo16, zero);
            __m128i hi16 = _mm_unpackhi_epi8(px, zero);
            __m128i lo32_2 = _mm_unpacklo_epi16(hi16, zero);

            __m128 f0 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_0), scale);
            __m128 f1 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_1), scale);
            __m128 f2 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_2), scale);

            // 存储 12 个 float
            _mm_storeu_ps(dst_row + x * 3 + 0, f0);
            _mm_storeu_ps(dst_row + x * 3 + 4, f1);
            _mm_storeu_ps(dst_row + x * 3 + 8, f2);
        }

        // 处理剩余像素
        for (; x < width; ++x) {
            int src_x = (width - 1 - x) * 3;
            int dst_x = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 0] * scalar_scale;
            dst_row[dst_x + 1] = src_row[src_x + 1] * scalar_scale;
            dst_row[dst_x + 2] = src_row[src_x + 2] * scalar_scale;
        }
    }
}
#endif

// ============================================================================
// 水平翻转并归一化 - AVX2 实现
// ============================================================================
#ifdef HAS_AVX2_IMPL
__declspec(noinline) static void FlipHorizontalAndNormalize_AVX2(const uint8_t* src, float* dst, int width, int height) {
    const __m256 scale = _mm256_set1_ps(1.0f / 255.0f);
    const float scalar_scale = 1.0f / 255.0f;

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        float* dst_row = dst + y * width * 3;

        int x = 0;
        // AVX2: 每次处理 8 个像素
        int simd_end = width - 8;

        for (; x <= simd_end; x += 8) {
            // 从源图像右侧读取 8 个像素 (翻转后的位置)
            int src_x = (width - 1 - x) * 3;

            // 手动反向加载 8 个像素 (24 字节)
            alignas(32) uint8_t pixels[32];
            for (int i = 0; i < 8; ++i) {
                int src_px = src_x - i * 3;
                pixels[i * 3 + 0] = src_row[src_px + 0];
                pixels[i * 3 + 1] = src_row[src_px + 1];
                pixels[i * 3 + 2] = src_row[src_px + 2];
            }

            // 加载并转换
            __m128i pixels_lo = _mm_loadu_si128((const __m128i*)pixels);
            __m128i pixels_hi = _mm_loadl_epi64((const __m128i*)(pixels + 16));

            // 使用 AVX2 的 _mm256_cvtepu8_epi32 转换
            __m256i i32_0 = _mm256_cvtepu8_epi32(pixels_lo);
            __m128i mid8 = _mm_srli_si128(pixels_lo, 8);
            __m256i i32_1 = _mm256_cvtepu8_epi32(mid8);
            __m256i i32_2 = _mm256_cvtepu8_epi32(pixels_hi);

            // int32 -> float 并乘以 scale
            __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_0), scale);
            __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_1), scale);
            __m256 f2 = _mm256_mul_ps(_mm256_cvtepi32_ps(i32_2), scale);

            // 存储 24 个 float
            _mm256_storeu_ps(dst_row + x * 3 + 0, f0);
            _mm256_storeu_ps(dst_row + x * 3 + 8, f1);
            _mm256_storeu_ps(dst_row + x * 3 + 16, f2);
        }

        // 处理剩余像素
        for (; x < width; ++x) {
            int src_x = (width - 1 - x) * 3;
            int dst_x = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 0] * scalar_scale;
            dst_row[dst_x + 1] = src_row[src_x + 1] * scalar_scale;
            dst_row[dst_x + 2] = src_row[src_x + 2] * scalar_scale;
        }
    }
}
#endif

// ============================================================================
// 水平翻转并归一化 - 统一接口
// ============================================================================
void FlipHorizontalAndNormalize(const uint8_t* src, float* dst, int width, int height) {
    if (!g_initialized) Init();

    SIMDMode effectiveMode = g_currentMode;

    if (effectiveMode == SIMDMode::Auto) {
#ifdef HAS_AVX2_IMPL
        if (g_hasAVX2) {
            FlipHorizontalAndNormalize_AVX2(src, dst, width, height);
            return;
        }
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
        if (g_hasSSE2) {
            FlipHorizontalAndNormalize_SSE(src, dst, width, height);
            return;
        }
#endif
        FlipHorizontalAndNormalize_Scalar(src, dst, width, height);
        return;
    }

    switch (effectiveMode) {
        case SIMDMode::AVX2:
#ifdef HAS_AVX2_IMPL
            if (g_hasAVX2) {
                FlipHorizontalAndNormalize_AVX2(src, dst, width, height);
                return;
            }
#endif
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
            if (g_hasSSE2) {
                FlipHorizontalAndNormalize_SSE(src, dst, width, height);
                return;
            }
#endif
            FlipHorizontalAndNormalize_Scalar(src, dst, width, height);
            break;

        case SIMDMode::SSE:
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
            if (g_hasSSE2) {
                FlipHorizontalAndNormalize_SSE(src, dst, width, height);
                return;
            }
#endif
            FlipHorizontalAndNormalize_Scalar(src, dst, width, height);
            break;

        case SIMDMode::Scalar:
        default:
            FlipHorizontalAndNormalize_Scalar(src, dst, width, height);
            break;
    }
}

} // namespace SIMDNormalize
