#include "SIMDNormalize.h"
#include "SIMDNormalizeDispatch.h"
#include "SIMDNormalizeKernels.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

void AssertEqual(const std::vector<float>& left, const std::vector<float>& right) {
    assert(left.size() == right.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
        assert(std::abs(left[index] - right[index]) < 0.000001f);
    }
}

void AssertEqual(const std::vector<std::uint8_t>& left, const std::vector<std::uint8_t>& right) {
    assert(left == right);
}

void VerifyDispatcherWithSyntheticFeatures() {
    using namespace SIMDNormalize::Internal;
    const CpuFeatureSet allX86{
        .sse2 = true, .ssse3 = true, .sse41 = true, .avx2 = true,
    };
    const auto automatic = KernelDispatcher::Select(SIMDMode::Auto, KernelOperation::Normalize, allX86);
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
    assert(automatic == KernelImplementation::AVX2);
#elif PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
    assert(automatic == KernelImplementation::SSE41);
#elif PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
    assert(automatic == KernelImplementation::SSSE3);
#elif PARTICLESATURN_SIMD_HAS_SSE2_KERNEL
    assert(automatic == KernelImplementation::SSE2);
#else
    assert(automatic == KernelImplementation::Scalar);
#endif

    const CpuFeatureSet sse2Only{.sse2 = true};
    const auto normalize = KernelDispatcher::Select(SIMDMode::Auto, KernelOperation::Normalize, sse2Only);
#if PARTICLESATURN_SIMD_HAS_SSE2_KERNEL
    assert(normalize == KernelImplementation::SSE2);
#else
    assert(normalize == KernelImplementation::Scalar);
#endif
    assert(KernelDispatcher::Select(SIMDMode::Auto, KernelOperation::FlipNormalize, sse2Only) ==
           KernelImplementation::Scalar);

    const CpuFeatureSet neonOnly{.neon = true};
    const auto neon = KernelDispatcher::Select(SIMDMode::Auto, KernelOperation::Normalize, neonOnly);
#if PARTICLESATURN_SIMD_HAS_NEON_KERNEL
    assert(neon == KernelImplementation::NEON);
#else
    assert(neon == KernelImplementation::Scalar);
#endif
    assert(KernelDispatcher::Select(SIMDMode::AVX2, KernelOperation::Normalize, {}) ==
           KernelImplementation::Scalar);
}

void VerifyModeAgainstScalar(SIMDMode mode, const std::vector<std::uint8_t>& pixels, int width, int height,
                             const std::vector<float>& scalarNormalized, const std::vector<float>& scalarFlipped,
                             const std::vector<std::uint8_t>& scalarConverted) {
    std::vector<float> normalized(pixels.size());
    std::vector<float> flipped(pixels.size());
    std::vector<std::uint8_t> converted(pixels.size());
    SIMDNormalize::SetMode(mode);
    SIMDNormalize::NormalizeRGB(pixels.data(), normalized.data(), static_cast<std::size_t>(width * height));
    SIMDNormalize::FlipHorizontalAndNormalize(pixels.data(), flipped.data(), width, height);
    SIMDNormalize::FlipHorizontalAndBGR2RGB(pixels.data(), converted.data(), width, height);
    AssertEqual(normalized, scalarNormalized);
    AssertEqual(flipped, scalarFlipped);
    AssertEqual(converted, scalarConverted);
}

} // namespace

int main() {
    VerifyDispatcherWithSyntheticFeatures();
    const CpuFeatureSet features = SIMDNormalize::GetCpuFeatures();
    assert(features.avx2 == SIMDNormalize::IsAVX2Supported());
    assert(features.sse2 == SIMDNormalize::IsSSE2Supported());
    assert(features.neon == SIMDNormalize::IsNEONSupported());
    assert(!features.avx2 || features.sse2);
    SIMDNormalize::SetMode(SIMDMode::Auto);
    assert(SIMDNormalize::GetCurrentImplementation() != nullptr);
    constexpr int width = 5;
    constexpr int height = 3;
    std::vector<std::uint8_t> pixels(width * height * 3);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<std::uint8_t>((index * 37U) % 256U);
    }

    std::vector<float> scalar(pixels.size());
    std::vector<float> automatic(pixels.size());
    SIMDNormalize::SetMode(SIMDMode::Scalar);
    SIMDNormalize::NormalizeRGB(pixels.data(), scalar.data(), width * height);
    std::vector<float> scalarFlipped(pixels.size());
    std::vector<std::uint8_t> scalarConverted(pixels.size());
    SIMDNormalize::FlipHorizontalAndNormalize(pixels.data(), scalarFlipped.data(), width, height);
    SIMDNormalize::FlipHorizontalAndBGR2RGB(pixels.data(), scalarConverted.data(), width, height);
    SIMDNormalize::SetMode(SIMDMode::Auto);
    SIMDNormalize::NormalizeRGB(pixels.data(), automatic.data(), width * height);
    AssertEqual(scalar, automatic);

    if (features.neon) {
        VerifyModeAgainstScalar(SIMDMode::NEON, pixels, width, height, scalar, scalarFlipped, scalarConverted);
    }
    if (features.sse2) {
        VerifyModeAgainstScalar(SIMDMode::SSE, pixels, width, height, scalar, scalarFlipped, scalarConverted);
    }
    if (features.sse41) {
        VerifyModeAgainstScalar(SIMDMode::SSE41, pixels, width, height, scalar, scalarFlipped, scalarConverted);
    }
    if (features.avx2) {
        VerifyModeAgainstScalar(SIMDMode::AVX2, pixels, width, height, scalar, scalarFlipped, scalarConverted);
    }

#if PARTICLESATURN_SIMD_HAS_SSE2_KERNEL
    {
        std::vector<float> normalized(pixels.size());
        SIMDNormalize::Kernels::NormalizeRGBSse2(pixels.data(), normalized.data(), width * height);
        AssertEqual(normalized, scalar);
    }
#endif
#if PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
    if (features.ssse3) {
        std::vector<float> normalized(pixels.size());
        std::vector<float> flippedKernel(pixels.size());
        std::vector<std::uint8_t> converted(pixels.size());
        SIMDNormalize::Kernels::NormalizeRGBSsse3(pixels.data(), normalized.data(), width * height);
        SIMDNormalize::Kernels::FlipHorizontalAndNormalizeSsse3(pixels.data(), flippedKernel.data(), width, height);
        SIMDNormalize::Kernels::FlipHorizontalAndBGR2RGBSsse3(pixels.data(), converted.data(), width, height);
        AssertEqual(normalized, scalar);
        AssertEqual(flippedKernel, scalarFlipped);
        AssertEqual(converted, scalarConverted);
    }
#endif
#if PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
    if (features.sse41) {
        std::vector<float> normalized(pixels.size());
        std::vector<float> flippedKernel(pixels.size());
        std::vector<std::uint8_t> converted(pixels.size());
        SIMDNormalize::Kernels::NormalizeRGBSse41(pixels.data(), normalized.data(), width * height);
        SIMDNormalize::Kernels::FlipHorizontalAndNormalizeSse41(pixels.data(), flippedKernel.data(), width, height);
        SIMDNormalize::Kernels::FlipHorizontalAndBGR2RGBSse41(pixels.data(), converted.data(), width, height);
        AssertEqual(normalized, scalar);
        AssertEqual(flippedKernel, scalarFlipped);
        AssertEqual(converted, scalarConverted);
    }
#endif
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
    if (features.avx2) {
        std::vector<float> normalized(pixels.size());
        std::vector<float> flippedKernel(pixels.size());
        std::vector<std::uint8_t> converted(pixels.size());
        SIMDNormalize::Kernels::NormalizeRGBAvx2(pixels.data(), normalized.data(), width * height);
        SIMDNormalize::Kernels::FlipHorizontalAndNormalizeAvx2(pixels.data(), flippedKernel.data(), width, height);
        SIMDNormalize::Kernels::FlipHorizontalAndBGR2RGBAvx2(pixels.data(), converted.data(), width, height);
        AssertEqual(normalized, scalar);
        AssertEqual(flippedKernel, scalarFlipped);
        AssertEqual(converted, scalarConverted);
    }
#endif

    for (const std::size_t count : {0U, 1U, 2U, 3U, 4U, 5U, 7U, 8U, 9U}) {
        std::vector<std::uint8_t> unaligned(count * 3U + 1U, 0U);
        for (std::size_t index = 0; index < count * 3U; ++index) {
            unaligned[index + 1U] = static_cast<std::uint8_t>((index * 19U) % 256U);
        }
        std::vector<float> expected(count * 3U);
        std::vector<float> actual(count * 3U);
        SIMDNormalize::SetMode(SIMDMode::Scalar);
        SIMDNormalize::NormalizeRGB(unaligned.data() + 1U, expected.data(), count);
        SIMDNormalize::SetMode(SIMDMode::Auto);
        SIMDNormalize::NormalizeRGB(unaligned.data() + 1U, actual.data(), count);
        AssertEqual(expected, actual);
    }

    std::vector<float> flipped(pixels.size());
    SIMDNormalize::FlipHorizontalAndNormalize(pixels.data(), flipped.data(), width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int source = (y * width + width - x - 1) * 3;
            const int destination = (y * width + x) * 3;
            for (int channel = 0; channel < 3; ++channel) {
                assert(std::abs(flipped[destination + channel] - pixels[source + channel] / 255.0f) < 0.000001f);
            }
        }
    }

    for (const int testWidth : {1, 2, 3, 4, 5, 7, 8, 9}) {
        constexpr int testHeight = 2;
        std::vector<std::uint8_t> source(static_cast<std::size_t>(testWidth * testHeight * 3 + 1), 0U);
        for (std::size_t index = 0; index < source.size() - 1U; ++index) {
            source[index + 1U] = static_cast<std::uint8_t>((index * 13U) % 256U);
        }
        std::vector<float> normalized(static_cast<std::size_t>(testWidth * testHeight * 3));
        std::vector<std::uint8_t> converted(static_cast<std::size_t>(testWidth * testHeight * 3));
        SIMDNormalize::SetMode(SIMDMode::Auto);
        SIMDNormalize::FlipHorizontalAndNormalize(source.data() + 1U, normalized.data(), testWidth, testHeight);
        SIMDNormalize::FlipHorizontalAndBGR2RGB(source.data() + 1U, converted.data(), testWidth, testHeight);
        for (int y = 0; y < testHeight; ++y) {
            for (int x = 0; x < testWidth; ++x) {
                const int sourceOffset = (y * testWidth + testWidth - x - 1) * 3;
                const int destinationOffset = (y * testWidth + x) * 3;
                assert(std::abs(normalized[destinationOffset] - source[sourceOffset + 1] / 255.0f) < 0.000001f);
                assert(converted[destinationOffset] == source[sourceOffset + 3]);
                assert(converted[destinationOffset + 1] == source[sourceOffset + 2]);
                assert(converted[destinationOffset + 2] == source[sourceOffset + 1]);
            }
        }
    }

#if defined(__aarch64__)
    assert(SIMDNormalize::IsNEONSupported());
#endif
    SIMDNormalize::SetMode(SIMDMode::Auto);
    if (!SIMDNormalize::IsAVX2Supported()) {
        SIMDNormalize::SetMode(SIMDMode::AVX2);
        assert(SIMDNormalize::GetMode() == SIMDMode::Auto);
    }
    if (!SIMDNormalize::IsSSE2Supported()) {
        SIMDNormalize::SetMode(SIMDMode::SSE);
        assert(SIMDNormalize::GetMode() == SIMDMode::Auto);
    }
    return 0;
}
