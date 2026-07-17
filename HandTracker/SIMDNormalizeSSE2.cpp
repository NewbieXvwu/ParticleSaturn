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

} // namespace

void NormalizeRGBSse2(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    const __m128 scale = _mm_set1_ps(1.0f / 255.0f);
    const __m128i zero = _mm_setzero_si128();
    std::size_t index = 0;
    const auto simdCount = (pixelCount / 4U) * 4U;
    for (; index < simdCount; index += 4U) {
        const __m128i pixels = Load12Bytes(src + index * 3U);
        const __m128i low16 = _mm_unpacklo_epi8(pixels, zero);
        const __m128i high16 = _mm_unpackhi_epi8(pixels, zero);
        _mm_storeu_ps(dst + index * 3U, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(low16, zero)), scale));
        _mm_storeu_ps(dst + index * 3U + 4U, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpackhi_epi16(low16, zero)), scale));
        _mm_storeu_ps(dst + index * 3U + 8U, _mm_mul_ps(_mm_cvtepi32_ps(_mm_unpacklo_epi16(high16, zero)), scale));
    }
    NormalizeRGBScalar(src + index * 3U, dst + index * 3U, pixelCount - index);
}

} // namespace SIMDNormalize::Kernels
#endif
