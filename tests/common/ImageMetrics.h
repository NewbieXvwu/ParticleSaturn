#pragma once

// 图像差异度量的唯一实现（AUDIT P2-9）：逐像素通道绝对差累加，
// 供视觉基线与 object shader 基线共用；阈值常量具名共享。
// 语义：MeanChannelDifference = 所有通道绝对差的均值（8-bit 尺度）；
// MismatchFraction = 单像素最大通道差超过阈值的像素占比。
#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ParticleSaturn::TestCommon {

// 判定"失配像素"的单通道差阈值（8-bit 尺度）。
inline constexpr int PerPixelChannelThreshold = 8;

class ImageDifferenceAccumulator {
public:
    void AddPixel(int r0, int g0, int b0, int r1, int g1, int b1) {
        const int dr = std::abs(r0 - r1);
        const int dg = std::abs(g0 - g1);
        const int db = std::abs(b0 - b1);
        totalChannelDifference_ += static_cast<std::uint64_t>(dr + dg + db);
        mismatchedPixels_ += std::max({dr, dg, db}) > PerPixelChannelThreshold;
        ++pixels_;
    }

    double MeanChannelDifference() const {
        return pixels_ == 0 ? 0.0
                            : static_cast<double>(totalChannelDifference_) / (3.0 * static_cast<double>(pixels_));
    }

    double MismatchFraction() const {
        return pixels_ == 0 ? 0.0 : static_cast<double>(mismatchedPixels_) / static_cast<double>(pixels_);
    }

private:
    std::uint64_t totalChannelDifference_ = 0;
    std::uint64_t mismatchedPixels_ = 0;
    std::uint64_t pixels_ = 0;
};

} // namespace ParticleSaturn::TestCommon
