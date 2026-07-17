// SIMDNormalize.cpp - SIMD 加速的图像归一化
// 支持 AVX2、SSE2 和标量回退

#include "SIMDNormalize.h"

#include <cstring>
#include <iostream>

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
#define PARTICLESATURN_X86_SIMD 1
#endif

#if defined(__aarch64__) || defined(__ARM_NEON)
#define PARTICLESATURN_NEON_SIMD 1
#endif

#if defined(PARTICLESATURN_X86_SIMD) && defined(_MSC_VER)
#include <intrin.h>
#elif defined(PARTICLESATURN_X86_SIMD)
#include <cpuid.h>
#endif

#if defined(PARTICLESATURN_X86_SIMD)
#include <immintrin.h>
#endif

#if defined(__SSSE3__)
#define PARTICLESATURN_HAS_SSSE3_IMPL 1
#endif

#if defined(PARTICLESATURN_NEON_SIMD)
#include <arm_neon.h>
#endif

namespace SIMDNormalize {

// CPU 特性检测结果（缓存）
static bool     g_initialized = false;
static bool     g_hasAVX2     = false;
static bool     g_hasSSE2     = false;
static bool     g_hasSSSE3    = false;
static bool     g_hasNEON     = false;
static SIMDMode g_currentMode = SIMDMode::Auto;

#if defined(PARTICLESATURN_X86_SIMD)
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

static std::uint64_t GetXCR0() {
#ifdef _MSC_VER
    return _xgetbv(0);
#else
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32U) | low;
#endif
}
#endif

void Init() {
    if (g_initialized) {
        return;
    }

#if defined(PARTICLESATURN_X86_SIMD)
    int info[4]{};
    // 检查 CPUID 支持
    GetCPUID(info, 0);
    int maxFunction = info[0];

    if (maxFunction >= 1) {
        GetCPUID(info, 1);
        // SSE2: EDX bit 26
        g_hasSSE2 = (info[3] & (1 << 26)) != 0;
        // SSSE3: ECX bit 9, required by _mm_shuffle_epi8.
        g_hasSSSE3 = (info[2] & (1 << 9)) != 0;
        const bool osXSave = (info[2] & (1 << 27)) != 0;
        const bool avx = (info[2] & (1 << 28)) != 0;
        if (osXSave && avx) {
            const auto xcr0 = GetXCR0();
            g_hasAVX2 = (xcr0 & 0x6U) == 0x6U;
        }
    }

    if (g_hasAVX2 && maxFunction >= 7) {
        GetCPUIDEx(info, 7, 0);
        // AVX2: EBX bit 5
        g_hasAVX2 = (info[1] & (1 << 5)) != 0;
    } else {
        g_hasAVX2 = false;
    }
#endif

#if defined(PARTICLESATURN_NEON_SIMD)
    g_hasNEON = true;
#endif

    g_initialized = true;

    std::cout << "[SIMD] CPU features detected - NEON: " << (g_hasNEON ? "Yes" : "No")
              << ", AVX2: " << (g_hasAVX2 ? "Yes" : "No")
              << ", SSE2: " << (g_hasSSE2 ? "Yes" : "No")
              << ", SSSE3: " << (g_hasSSSE3 ? "Yes" : "No") << std::endl;
}

void SetMode(SIMDMode mode) {
    g_currentMode = mode;
    std::cout << "[SIMD] Mode set to: " << GetCurrentImplementation() << std::endl;
}

SIMDMode GetMode() {
    return g_currentMode;
}

bool IsAVX2Supported() {
    if (!g_initialized) {
        Init();
    }
    return g_hasAVX2;
}

bool IsSSE2Supported() {
    if (!g_initialized) {
        Init();
    }
    return g_hasSSE2;
}

bool IsNEONSupported() {
    if (!g_initialized) {
        Init();
    }
    return g_hasNEON;
}

const char* GetCurrentImplementation() {
    if (!g_initialized) {
        Init();
    }

    switch (g_currentMode) {
    case SIMDMode::NEON:
        return g_hasNEON ? "NEON (forced)" : "NEON (unavailable, using scalar)";
    case SIMDMode::AVX2:
        return g_hasAVX2 ? "AVX2 (forced)" : "AVX2 (unavailable, using fallback)";
    case SIMDMode::SSE:
        return g_hasSSE2 ? "SSE (forced)" : "SSE (unavailable, using scalar)";
    case SIMDMode::Scalar:
        return "Scalar (forced)";
    case SIMDMode::Auto:
    default:
        if (g_hasNEON) {
            return "NEON (auto)";
        }
        if (g_hasAVX2) {
            return "AVX2 (auto)";
        }
        if (g_hasSSE2) {
            return "SSE (auto)";
        }
        return "Scalar (auto)";
    }
}

// ============================================================================
// 标量实现
// ============================================================================
static void NormalizeRGB_Scalar(const uint8_t* src, float* dst, size_t pixel_count) {
    const float scale = 1.0f / 255.0f;
    size_t      total = pixel_count * 3;
    for (size_t i = 0; i < total; ++i) {
        dst[i] = src[i] * scale;
    }
}

#if defined(PARTICLESATURN_NEON_SIMD)
static void NormalizeRGB_NEON(const uint8_t* src, float* dst, size_t pixel_count) {
    constexpr float scale = 1.0f / 255.0f;
    size_t i = 0;
    const size_t channelCount = pixel_count * 3;
    for (; i + 16 <= channelCount; i += 16) {
        const uint8x16_t bytes = vld1q_u8(src + i);
        const uint16x8_t low16 = vmovl_u8(vget_low_u8(bytes));
        const uint16x8_t high16 = vmovl_u8(vget_high_u8(bytes));
        const uint32x4_t low32a = vmovl_u16(vget_low_u16(low16));
        const uint32x4_t low32b = vmovl_u16(vget_high_u16(low16));
        const uint32x4_t high32a = vmovl_u16(vget_low_u16(high16));
        const uint32x4_t high32b = vmovl_u16(vget_high_u16(high16));
        vst1q_f32(dst + i, vmulq_n_f32(vcvtq_f32_u32(low32a), scale));
        vst1q_f32(dst + i + 4, vmulq_n_f32(vcvtq_f32_u32(low32b), scale));
        vst1q_f32(dst + i + 8, vmulq_n_f32(vcvtq_f32_u32(high32a), scale));
        vst1q_f32(dst + i + 12, vmulq_n_f32(vcvtq_f32_u32(high32b), scale));
    }
    for (; i < channelCount; ++i) {
        dst[i] = src[i] * scale;
    }
}
#endif

// ============================================================================
// SSE 实现 (一次处理 4 个像素 = 12 个通道)
// ============================================================================
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
static __m128i Load12Bytes(const uint8_t* source) {
    std::uint32_t tail = 0;
    std::memcpy(&tail, source + 8, sizeof(tail));
    return _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source)), _mm_cvtsi32_si128(static_cast<int>(tail)));
}

static void NormalizeRGB_SSE(const uint8_t* src, float* dst, size_t pixel_count) {
    const __m128  scale = _mm_set1_ps(1.0f / 255.0f);
    const __m128i zero  = _mm_setzero_si128();

    size_t i = 0;
    // 每次处理 4 个像素 (12 bytes -> 12 floats)
    // 但 SSE 寄存器是 128 位 (4 floats)，所以需要 3 次存储
    size_t simd_count = (pixel_count / 4) * 4;

    for (; i < simd_count; i += 4) {
        // 精确读取四个 RGB 像素，最后一个 4 字节块不能跨越输入边界。
        __m128i pixels = Load12Bytes(src + i * 3);

        // 解包 uint8 -> int16 -> int32
        __m128i lo16 = _mm_unpacklo_epi8(pixels, zero); // 前 8 个 uint8 -> 8 个 int16

        // int16 -> int32 (前 4 个)
        __m128i lo32_0 = _mm_unpacklo_epi16(lo16, zero); // 4 个 int32
        __m128i lo32_1 = _mm_unpackhi_epi16(lo16, zero); // 4 个 int32

        // 再解包一次获取第三组
        __m128i hi16   = _mm_unpackhi_epi8(pixels, zero);
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
        __m128i pixels_lo = _mm_loadu_si128((const __m128i*)(src + i * 3));      // 前 16 字节
        __m128i pixels_hi = _mm_loadl_epi64((const __m128i*)(src + i * 3 + 16)); // 后 8 字节

        // 组合成 256 位
        __m256i pixels = _mm256_set_m128i(pixels_hi, pixels_lo);

        // 使用 AVX2 的 _mm256_cvtepu8_epi32 一次将 8 个 uint8 转为 8 个 int32
        // 但我们有 24 个通道，需要分 3 组处理

        // 提取前 8 个字节 -> 8 个 int32
        __m256i i32_0 = _mm256_cvtepu8_epi32(pixels_lo);

        // 提取中间 8 个字节
        __m128i mid8  = _mm_srli_si128(pixels_lo, 8); // 移位获取第 8-15 字节
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
    if (!g_initialized) {
        Init();
    }

    // 根据模式选择实现
    SIMDMode effectiveMode = g_currentMode;

    if (effectiveMode == SIMDMode::Auto) {
        // 自动选择最佳实现
#if defined(PARTICLESATURN_NEON_SIMD)
        if (g_hasNEON) {
            NormalizeRGB_NEON(src, dst, pixel_count);
            return;
        }
#endif
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
    case SIMDMode::NEON:
#if defined(PARTICLESATURN_NEON_SIMD)
        if (g_hasNEON) {
            NormalizeRGB_NEON(src, dst, pixel_count);
            return;
        }
#endif
        NormalizeRGB_Scalar(src, dst, pixel_count);
        break;
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
        float*         dst_row = dst + y * width * 3;
        for (int x = 0; x < width; ++x) {
            int src_x          = (width - 1 - x) * 3;
            int dst_x          = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 0] * scale;
            dst_row[dst_x + 1] = src_row[src_x + 1] * scale;
            dst_row[dst_x + 2] = src_row[src_x + 2] * scale;
        }
    }
}

// ============================================================================
// 水平翻转并归一化 - SSE 实现
// ============================================================================
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
static void FlipHorizontalAndNormalize_SSE(const uint8_t* src, float* dst, int width, int height) {
    const __m128  scale        = _mm_set1_ps(1.0f / 255.0f);
    const __m128i zero         = _mm_setzero_si128();
    const float   scalar_scale = 1.0f / 255.0f;

    // Shuffle mask: P3 P2 P1 P0 -> P0 P1 P2 P3
    // RGB RGB RGB RGB -> RGB RGB RGB RGB
    // P0: 9,10,11. P1: 6,7,8. P2: 3,4,5. P3: 0,1,2.
    const __m128i shuffle_mask = _mm_setr_epi8(9, 10, 11, 6, 7, 8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        float*         dst_row = dst + y * width * 3;

        int x        = 0;
        int simd_end = width - 4;

        for (; x <= simd_end; x += 4) {
            const int src_offset = (width - 4 - x) * 3;
            __m128i px = Load12Bytes(src_row + src_offset);

            // Shuffle to reverse order
            px = _mm_shuffle_epi8(px, shuffle_mask);

            // Unpack and convert
            __m128i lo16   = _mm_unpacklo_epi8(px, zero);
            __m128i lo32_0 = _mm_unpacklo_epi16(lo16, zero);
            __m128i lo32_1 = _mm_unpackhi_epi16(lo16, zero);
            __m128i hi16   = _mm_unpackhi_epi8(px, zero);
            __m128i lo32_2 = _mm_unpacklo_epi16(hi16, zero);

            __m128 f0 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_0), scale);
            __m128 f1 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_1), scale);
            __m128 f2 = _mm_mul_ps(_mm_cvtepi32_ps(lo32_2), scale);

            _mm_storeu_ps(dst_row + x * 3 + 0, f0);
            _mm_storeu_ps(dst_row + x * 3 + 4, f1);
            _mm_storeu_ps(dst_row + x * 3 + 8, f2);
        }

        // Fallback
        for (; x < width; ++x) {
            int src_x          = (width - 1 - x) * 3;
            int dst_x          = x * 3;
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
__declspec(noinline) static void FlipHorizontalAndNormalize_AVX2(const uint8_t* src, float* dst, int width,
                                                                 int height) {
    const __m256 scale        = _mm256_set1_ps(1.0f / 255.0f);
    const float  scalar_scale = 1.0f / 255.0f;

    const __m256i shuffle_mask = _mm256_setr_epi8(9, 10, 11, 6, 7, 8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1, 9, 10, 11, 6, 7,
                                                  8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1);

    // Permute to pack: Hi (dst 0..3) then Lo (dst 4..7)
    // Indices in 32-bit units: Hi(4,5,6), Lo(0,1,2)
    const __m256i permute_mask = _mm256_setr_epi32(4, 5, 6, 0, 1, 2, 0, 0);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        float*         dst_row = dst + y * width * 3;

        int x        = 0;
        int simd_end = width - 8;

        for (; x <= simd_end; x += 8) {
            int src_offset_hi = (width - 4 - x) * 3; // Chunk 1 (dst 0..3)
            int src_offset_lo = (width - 8 - x) * 3; // Chunk 2 (dst 4..7)

            __m128i v_lo = _mm_loadu_si128((const __m128i*)(src_row + src_offset_lo));
            __m128i v_hi = _mm_loadu_si128((const __m128i*)(src_row + src_offset_hi));

            __m256i ymm = _mm256_inserti128_si256(_mm256_castsi128_si256(v_lo), v_hi, 1);

            ymm = _mm256_shuffle_epi8(ymm, shuffle_mask);
            ymm = _mm256_permutevar8x32_epi32(ymm, permute_mask);

            // Now lower 24 bytes contain 8 pixels in order
            // We need to convert bytes 0-7, 8-15, 16-23 to floats

            // 0-7
            __m256i i0 = _mm256_cvtepu8_epi32(_mm256_castsi256_si128(ymm));

            // 8-15
            // Can't use srli_si256 on AVX register across lanes, but here all data is in low 256 bits?
            // No, data is 24 bytes. 0-15 in Lane 0. 16-23 in Lane 1.
            // Wait, permutevar8x32 result:
            // Indices 4,5,6 from original (Lane 1 valid part) -> Output 0,1,2 (Lane 0)
            // Indices 0,1,2 from original (Lane 0 valid part) -> Output 3,4,5 (Lane 0 high, Lane 1 low)
            // Output layout: [Hi_0, Hi_1, Hi_2, Lo_0] [Lo_1, Lo_2, 0, 0]
            // Lane 0: 16 bytes. Lane 1: 16 bytes.
            // Data is contiguous 24 bytes.
            // Byte 0-15 in Lane 0. Byte 16-23 in Lane 1.

            // i0 (bytes 0-7) -> Correct.

            // i1 (bytes 8-15)
            // Extract Lane 0, shift right 8 bytes
            __m128i lane0 = _mm256_castsi256_si128(ymm);
            __m128i mid   = _mm_srli_si128(lane0, 8);
            __m256i i1    = _mm256_cvtepu8_epi32(mid);

            // i2 (bytes 16-23)
            // Extract Lane 1
            __m128i lane1 = _mm256_extracti128_si256(ymm, 1);
            __m256i i2    = _mm256_cvtepu8_epi32(lane1);

            __m256 f0 = _mm256_mul_ps(_mm256_cvtepi32_ps(i0), scale);
            __m256 f1 = _mm256_mul_ps(_mm256_cvtepi32_ps(i1), scale);
            __m256 f2 = _mm256_mul_ps(_mm256_cvtepi32_ps(i2), scale);

            _mm256_storeu_ps(dst_row + x * 3 + 0, f0);
            _mm256_storeu_ps(dst_row + x * 3 + 8, f1);
            _mm256_storeu_ps(dst_row + x * 3 + 16, f2);
        }

        for (; x < width; ++x) {
            int src_x          = (width - 1 - x) * 3;
            int dst_x          = x * 3;
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
    if (!g_initialized) {
        Init();
    }

    SIMDMode effectiveMode = g_currentMode;

    if (effectiveMode == SIMDMode::Auto) {
#ifdef HAS_AVX2_IMPL
        if (g_hasAVX2) {
            FlipHorizontalAndNormalize_AVX2(src, dst, width, height);
            return;
        }
#endif
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
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
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
            FlipHorizontalAndNormalize_SSE(src, dst, width, height);
            return;
        }
#endif
        FlipHorizontalAndNormalize_Scalar(src, dst, width, height);
        break;

    case SIMDMode::SSE:
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
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

// ============================================================================
// 水平翻转并 BGR 转 RGB - 标量实现
// ============================================================================
static void FlipHorizontalAndBGR2RGB_Scalar(const uint8_t* src, uint8_t* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        uint8_t*       dst_row = dst + y * width * 3;
        for (int x = 0; x < width; ++x) {
            int src_x = (width - 1 - x) * 3;
            int dst_x = x * 3;
            // BGR to RGB and Flip
            dst_row[dst_x + 0] = src_row[src_x + 2]; // R
            dst_row[dst_x + 1] = src_row[src_x + 1]; // G
            dst_row[dst_x + 2] = src_row[src_x + 0]; // B
        }
    }
}

// ============================================================================
// 水平翻转并 BGR 转 RGB - SSE 实现
// ============================================================================
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
static void FlipHorizontalAndBGR2RGB_SSE(const uint8_t* src, uint8_t* dst, int width, int height) {
    // Mask: Reverse 12 bytes completely.
    // BGR BGR BGR BGR -> (Reverse) -> R G B R G B ...
    const __m128i shuffle_mask = _mm_setr_epi8(11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        uint8_t*       dst_row = dst + y * width * 3;

        int x        = 0;
        int simd_end = width - 4;

        for (; x <= simd_end; x += 4) {
            const int src_offset = (width - 4 - x) * 3;
            __m128i px = Load12Bytes(src_row + src_offset);

            px = _mm_shuffle_epi8(px, shuffle_mask);

            // Store 12 bytes. We can't use storeu_si128 (16 bytes) if buffer is tight,
            // but usually safe except end of buffer.
            // Using 64-bit store + 32-bit store is safer.
            _mm_storel_epi64((__m128i*)(dst_row + x * 3), px);
            _mm_storeu_si32((void*)(dst_row + x * 3 + 8), _mm_bsrli_si128(px, 8));
        }

        for (; x < width; ++x) {
            int src_x          = (width - 1 - x) * 3;
            int dst_x          = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 2];
            dst_row[dst_x + 1] = src_row[src_x + 1];
            dst_row[dst_x + 2] = src_row[src_x + 0];
        }
    }
}
#endif

// ============================================================================
// 水平翻转并 BGR 转 RGB - AVX2 实现
// ============================================================================
#ifdef HAS_AVX2_IMPL
__declspec(noinline) static void FlipHorizontalAndBGR2RGB_AVX2(const uint8_t* src, uint8_t* dst, int width,
                                                               int height) {
    // Mask: Reverse 12 bytes in each lane
    const __m256i shuffle_mask = _mm256_setr_epi8(11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1, 11, 10, 9, 8, 7,
                                                  6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1);

    // Permute to pack: Hi (dst 0..3) then Lo (dst 4..7)
    // We want Hi_2, Lo_0, Lo_1, Lo_2 to form "V" (dst 8..23)
    // V layout: [Hi(8-11) | Lo(0-11) | junk]
    // Indices (32-bit): Hi_2 (6), Lo_0 (0), Lo_1 (1), Lo_2 (2)
    const __m256i v_perm_mask = _mm256_setr_epi32(6, 0, 1, 2, 0, 0, 0, 0);

    for (int y = 0; y < height; ++y) {
        const uint8_t* src_row = src + y * width * 3;
        uint8_t*       dst_row = dst + y * width * 3;

        int x        = 0;
        int simd_end = width - 8;

        for (; x <= simd_end; x += 8) {
            int src_offset_hi = (width - 4 - x) * 3;
            int src_offset_lo = (width - 8 - x) * 3;

            __m128i v_lo = _mm_loadu_si128((const __m128i*)(src_row + src_offset_lo));
            __m128i v_hi = _mm_loadu_si128((const __m128i*)(src_row + src_offset_hi));

            __m256i ymm = _mm256_inserti128_si256(_mm256_castsi128_si256(v_lo), v_hi, 1);

            ymm = _mm256_shuffle_epi8(ymm, shuffle_mask);

            // Store strategy:
            // 1. Store Hi (16 bytes) at dst. Valid: 0-11. Garbage: 12-15.
            // 2. Construct V containing correct 8-15 (from Hi and Lo) and 16-23 (from Lo).
            //    V = [Hi_2 | Lo_0 | Lo_1 | Lo_2] (16 bytes of interest)
            // 3. Store V (16 bytes) at dst+8.
            //    Overwrites 8-15 with correct data. Writes 16-23 correctly. Writes 24-27 garbage (handled by next iter
            //    or logic).

            // Construct V
            __m256i v_256 = _mm256_permutevar8x32_epi32(ymm, v_perm_mask);
            __m128i v     = _mm256_castsi256_si128(v_256);
            __m128i hi    = _mm256_extracti128_si256(ymm, 1);

            _mm_storeu_si128((__m128i*)(dst_row + x * 3), hi);
            _mm_storeu_si128((__m128i*)(dst_row + x * 3 + 8), v);
        }

        for (; x < width; ++x) {
            int src_x          = (width - 1 - x) * 3;
            int dst_x          = x * 3;
            dst_row[dst_x + 0] = src_row[src_x + 2];
            dst_row[dst_x + 1] = src_row[src_x + 1];
            dst_row[dst_x + 2] = src_row[src_x + 0];
        }
    }
}
#endif

void FlipHorizontalAndBGR2RGB(const uint8_t* src, uint8_t* dst, int width, int height) {
    if (!g_initialized) {
        Init();
    }

    SIMDMode effectiveMode = g_currentMode;

    if (effectiveMode == SIMDMode::Auto) {
#ifdef HAS_AVX2_IMPL
        if (g_hasAVX2) {
            FlipHorizontalAndBGR2RGB_AVX2(src, dst, width, height);
            return;
        }
#endif
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
            FlipHorizontalAndBGR2RGB_SSE(src, dst, width, height);
            return;
        }
#endif
        FlipHorizontalAndBGR2RGB_Scalar(src, dst, width, height);
        return;
    }

    switch (effectiveMode) {
    case SIMDMode::AVX2:
#ifdef HAS_AVX2_IMPL
        if (g_hasAVX2) {
            FlipHorizontalAndBGR2RGB_AVX2(src, dst, width, height);
            return;
        }
#endif
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
            FlipHorizontalAndBGR2RGB_SSE(src, dst, width, height);
            return;
        }
#endif
        FlipHorizontalAndBGR2RGB_Scalar(src, dst, width, height);
        break;

    case SIMDMode::SSE:
#if defined(PARTICLESATURN_HAS_SSSE3_IMPL)
        if (g_hasSSSE3) {
            FlipHorizontalAndBGR2RGB_SSE(src, dst, width, height);
            return;
        }
#endif
        FlipHorizontalAndBGR2RGB_Scalar(src, dst, width, height);
        break;

    case SIMDMode::Scalar:
    default:
        FlipHorizontalAndBGR2RGB_Scalar(src, dst, width, height);
        break;
    }
}

} // namespace SIMDNormalize
