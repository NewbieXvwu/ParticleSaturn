#include "SIMDNormalize.h"

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

} // namespace

int main() {
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
    SIMDNormalize::SetMode(SIMDMode::Auto);
    SIMDNormalize::NormalizeRGB(pixels.data(), automatic.data(), width * height);
    AssertEqual(scalar, automatic);

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

#if defined(__aarch64__)
    assert(SIMDNormalize::IsNEONSupported());
#endif
    return 0;
}
