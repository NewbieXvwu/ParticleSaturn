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

void StoreNormalized12(__m128i bytes, float* destination) {
    const __m128 scale = _mm_set1_ps(1.0f / 255.0f);
    _mm_storeu_ps(destination, _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(bytes)), scale));
    _mm_storeu_ps(destination + 4, _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_srli_si128(bytes, 4))), scale));
    _mm_storeu_ps(destination + 8, _mm_mul_ps(_mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_srli_si128(bytes, 8))), scale));
}

constexpr float kScale = 1.0f / 255.0f;

} // namespace

void NormalizeRGBSse41(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    std::size_t index = 0;
    for (; index + 4U <= pixelCount; index += 4U) StoreNormalized12(Load12Bytes(src + index * 3U), dst + index * 3U);
    NormalizeRGBScalar(src + index * 3U, dst + index * 3U, pixelCount - index);
}

void FlipHorizontalAndNormalizeSse41(const std::uint8_t* src, float* dst, int width, int height) {
    const __m128i reverseRgb = _mm_setr_epi8(9, 10, 11, 6, 7, 8, 3, 4, 5, 0, 1, 2, -1, -1, -1, -1);
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow = src + y * width * 3;
        auto* destinationRow = dst + y * width * 3;
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            StoreNormalized12(_mm_shuffle_epi8(Load12Bytes(sourceRow + (width - x - 4) * 3), reverseRgb), destinationRow + x * 3);
        }
        for (; x < width; ++x) {
            const int sourceOffset = (width - x - 1) * 3;
            const int destinationOffset = x * 3;
            destinationRow[destinationOffset] = sourceRow[sourceOffset] * kScale;
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1] * kScale;
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset + 2] * kScale;
        }
    }
}

void FlipHorizontalAndBGR2RGBSse41(const std::uint8_t* src, std::uint8_t* dst, int width, int height) {
    const __m128i reverseBgrToRgb = _mm_setr_epi8(11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, -1, -1, -1, -1);
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow = src + y * width * 3;
        auto* destinationRow = dst + y * width * 3;
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            const __m128i bytes = _mm_shuffle_epi8(Load12Bytes(sourceRow + (width - x - 4) * 3), reverseBgrToRgb);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(destinationRow + x * 3), bytes);
            std::uint32_t tail = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_srli_si128(bytes, 8)));
            std::memcpy(destinationRow + x * 3 + 8, &tail, sizeof(tail));
        }
        for (; x < width; ++x) {
            const int sourceOffset = (width - x - 1) * 3;
            const int destinationOffset = x * 3;
            destinationRow[destinationOffset] = sourceRow[sourceOffset + 2];
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1];
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset];
        }
    }
}

} // namespace SIMDNormalize::Kernels
#endif
