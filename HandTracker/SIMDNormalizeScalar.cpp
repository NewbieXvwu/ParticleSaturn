#include "SIMDNormalizeKernels.h"

namespace SIMDNormalize::Kernels {

void NormalizeRGBScalar(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    constexpr float scale = 1.0f / 255.0f;
    for (std::size_t index = 0; index < pixelCount; ++index) {
        dst[index * 3 + 0] = src[index * 3 + 0] * scale;
        dst[index * 3 + 1] = src[index * 3 + 1] * scale;
        dst[index * 3 + 2] = src[index * 3 + 2] * scale;
    }
}

void FlipHorizontalAndNormalizeScalar(const std::uint8_t* src, float* dst, int width, int height) {
    constexpr float scale = 1.0f / 255.0f;
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow      = src + y * width * 3;
        auto*       destinationRow = dst + y * width * 3;
        for (int x = 0; x < width; ++x) {
            const int sourceOffset                = (width - x - 1) * 3;
            const int destinationOffset           = x * 3;
            destinationRow[destinationOffset + 0] = sourceRow[sourceOffset + 0] * scale;
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1] * scale;
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset + 2] * scale;
        }
    }
}

void FlipHorizontalAndBGR2RGBScalar(const std::uint8_t* src, std::uint8_t* dst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const auto* sourceRow      = src + y * width * 3;
        auto*       destinationRow = dst + y * width * 3;
        for (int x = 0; x < width; ++x) {
            const int sourceOffset                = (width - x - 1) * 3;
            const int destinationOffset           = x * 3;
            destinationRow[destinationOffset + 0] = sourceRow[sourceOffset + 2];
            destinationRow[destinationOffset + 1] = sourceRow[sourceOffset + 1];
            destinationRow[destinationOffset + 2] = sourceRow[sourceOffset + 0];
        }
    }
}

} // namespace SIMDNormalize::Kernels
