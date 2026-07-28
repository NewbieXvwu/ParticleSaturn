#include "SIMDNormalizeKernels.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
#include <cstring>
#include <immintrin.h>

namespace SIMDNormalize::Kernels {
namespace {

__m128i Load12Bytes(const std::uint8_t* source) {
    std::uint32_t tail = 0;
    std::memcpy(&tail, source + 8, sizeof(tail));
    return _mm_unpacklo_epi64(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(source)),
                              _mm_cvtsi32_si128(static_cast<int>(tail)));
}

void StoreNormalized24(__m256i bytes, float* destination) {
    const __m256  scale = _mm256_set1_ps(1.0f / 255.0f);
    const __m128i low   = _mm256_castsi256_si128(bytes);
    const __m128i high  = _mm256_extracti128_si256(bytes, 1);
    _mm256_storeu_ps(destination, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(low)), scale));
    _mm256_storeu_ps(destination + 8,
                     _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(_mm_srli_si128(low, 8))), scale));
    _mm256_storeu_ps(destination + 16, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepu8_epi32(high)), scale));
}

constexpr float kScale = 1.0f / 255.0f;

__m256i Load24(const std::uint8_t* source) {
    const __m128i first16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(source));
    const __m128i last8   = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(source + 16));
    return _mm256_inserti128_si256(_mm256_castsi128_si256(first16), last8, 1);
}

} // namespace

void NormalizeRGBAvx2(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    std::size_t index = 0;
    for (; index + 8U <= pixelCount; index += 8U) {
        StoreNormalized24(Load24(src + index * 3U), dst + index * 3U);
    }
    NormalizeRGBScalar(src + index * 3U, dst + index * 3U, pixelCount - index);
}

void FlipHorizontalAndNormalizeAvx2(const std::uint8_t* src, float* dst, int width, int height) {
    const __m256i reverseRgb   = _mm256_setr_epi8(9, 10, 11, 6, 7, 8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1, 9, 10, 11, 6, 7,
                                                  8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1);
    const __m256i packReversed = _mm256_setr_epi32(4, 5, 6, 0, 1, 2, 0, 0);
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow      = src + y * width * 3;
        auto*       destinationRow = dst + y * width * 3;
        int         x              = 0;
        for (; x + 8 <= width; x += 8) {
            const __m128i low   = Load12Bytes(sourceRow + (width - x - 8) * 3);
            const __m128i high  = Load12Bytes(sourceRow + (width - x - 4) * 3);
            __m256i       bytes = _mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1);
            bytes               = _mm256_permutevar8x32_epi32(_mm256_shuffle_epi8(bytes, reverseRgb), packReversed);
            StoreNormalized24(bytes, destinationRow + x * 3);
        }
        for (; x < width; ++x) {
            const int sourceOffset                = (width - x - 1) * 3;
            const int destinationOffset           = x * 3;
            destinationRow[destinationOffset]     = sourceRow[sourceOffset] * kScale;
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1] * kScale;
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset + 2] * kScale;
        }
    }
}

void FlipHorizontalAndBGR2RGBAvx2(const std::uint8_t* src, std::uint8_t* dst, int width, int height) {
    const __m256i reverseBgrToRgb = _mm256_setr_epi8(11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1, 11, 10, 9, 8,
                                                     7, 6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1);
    const __m256i packBgrTail     = _mm256_setr_epi32(6, 0, 1, 2, 0, 0, 0, 0);
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow      = src + y * width * 3;
        auto*       destinationRow = dst + y * width * 3;
        int         x              = 0;
        for (; x + 8 <= width; x += 8) {
            const __m128i low  = Load12Bytes(sourceRow + (width - x - 8) * 3);
            const __m128i high = Load12Bytes(sourceRow + (width - x - 4) * 3);
            const __m256i bytes =
                _mm256_shuffle_epi8(_mm256_inserti128_si256(_mm256_castsi128_si256(low), high, 1), reverseBgrToRgb);
            const __m128i first8      = _mm256_extracti128_si256(bytes, 1);
            const __m128i remaining16 = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(bytes, packBgrTail));
            _mm_storel_epi64(reinterpret_cast<__m128i*>(destinationRow + x * 3), first8);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(destinationRow + x * 3 + 8), remaining16);
        }
        for (; x < width; ++x) {
            const int sourceOffset                = (width - x - 1) * 3;
            const int destinationOffset           = x * 3;
            destinationRow[destinationOffset]     = sourceRow[sourceOffset + 2];
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1];
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset];
        }
    }
}

} // namespace SIMDNormalize::Kernels
#endif
