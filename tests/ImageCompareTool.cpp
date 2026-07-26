// 对比模式核心工具（TODO P4，D-001 测量做成功能）：
// 读取参考与候选 PPM（各后端 PARTICLESATURN_CAPTURE_BASELINE 的确定性捕获），
// 输出共享度量（均值通道差/失配占比，tests/common/ImageMetrics.h 唯一实现）、
// 逐像素差异热力图与 参考|候选|热力图 并排图。
// 用法：ImageCompareTool <reference.ppm> <candidate.ppm> <output-prefix>
//   生成 <prefix>-heatmap.ppm 与 <prefix>-sidebyside.ppm，度量写 stdout。
// 退出码：0 = 成功产出（不设阈值判定——判定归 ctest/调用方）。

#include "common/ImageMetrics.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // RGB24
};

std::string ReadToken(std::istream& input) {
    std::string token;
    char character = '\0';
    while (input.get(character)) {
        if (character == '#') {
            input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        } else if (!std::isspace(static_cast<unsigned char>(character))) {
            token += character;
            break;
        }
    }
    while (input.get(character) && !std::isspace(static_cast<unsigned char>(character))) token += character;
    return token;
}

bool ReadPpm(const char* path, Image& image) {
    std::ifstream input{path, std::ios::binary};
    if (!input || ReadToken(input) != "P6") return false;
    const auto width = ReadToken(input);
    const auto height = ReadToken(input);
    const auto maximum = ReadToken(input);
    if (width.empty() || height.empty() || maximum != "255") return false;
    image.width = static_cast<std::uint32_t>(std::stoul(width));
    image.height = static_cast<std::uint32_t>(std::stoul(height));
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 3U);
    input.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
    return input.good() || input.eof();
}

bool WritePpm(const std::string& path, const Image& image) {
    std::ofstream output{path, std::ios::binary};
    if (!output) return false;
    output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    output.write(reinterpret_cast<const char*>(image.pixels.data()),
                 static_cast<std::streamsize>(image.pixels.size()));
    return output.good();
}

// 黑→蓝→红→黄 热力渐变；输入 0..255 的单像素最大通道差。
void HeatColor(int difference, std::uint8_t& red, std::uint8_t& green, std::uint8_t& blue) {
    const float t = std::clamp(static_cast<float>(difference) / 64.0f, 0.0f, 1.0f);  // 64+ 视为饱和
    if (t < 0.33f) {
        const float s = t / 0.33f;
        red = 0; green = 0; blue = static_cast<std::uint8_t>(s * 255.0f);
    } else if (t < 0.66f) {
        const float s = (t - 0.33f) / 0.33f;
        red = static_cast<std::uint8_t>(s * 255.0f); green = 0;
        blue = static_cast<std::uint8_t>((1.0f - s) * 255.0f);
    } else {
        const float s = (t - 0.66f) / 0.34f;
        red = 255; green = static_cast<std::uint8_t>(s * 255.0f); blue = 0;
    }
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <reference.ppm> <candidate.ppm> <output-prefix>\n", argv[0]);
        return 2;
    }
    Image reference;
    Image candidate;
    if (!ReadPpm(argv[1], reference)) {
        std::fprintf(stderr, "unable to read reference %s\n", argv[1]);
        return 2;
    }
    if (!ReadPpm(argv[2], candidate)) {
        std::fprintf(stderr, "unable to read candidate %s\n", argv[2]);
        return 2;
    }
    if (reference.width != candidate.width || reference.height != candidate.height) {
        std::fprintf(stderr, "size mismatch: %ux%u vs %ux%u\n", reference.width, reference.height,
                     candidate.width, candidate.height);
        return 2;
    }

    ParticleSaturn::TestCommon::ImageDifferenceAccumulator accumulator;
    Image heatmap{reference.width, reference.height,
                  std::vector<std::uint8_t>(reference.pixels.size())};
    for (std::size_t pixel = 0; pixel < reference.pixels.size(); pixel += 3U) {
        const int r0 = reference.pixels[pixel];
        const int g0 = reference.pixels[pixel + 1U];
        const int b0 = reference.pixels[pixel + 2U];
        const int r1 = candidate.pixels[pixel];
        const int g1 = candidate.pixels[pixel + 1U];
        const int b1 = candidate.pixels[pixel + 2U];
        accumulator.AddPixel(r0, g0, b0, r1, g1, b1);
        const int maxDiff = std::max({std::abs(r0 - r1), std::abs(g0 - g1), std::abs(b0 - b1)});
        HeatColor(maxDiff, heatmap.pixels[pixel], heatmap.pixels[pixel + 1U], heatmap.pixels[pixel + 2U]);
    }

    Image sideBySide{reference.width * 3U, reference.height,
                     std::vector<std::uint8_t>(static_cast<std::size_t>(reference.width) * 3U * reference.height * 3U)};
    for (std::uint32_t row = 0; row < reference.height; ++row) {
        const std::size_t sourceRow = static_cast<std::size_t>(row) * reference.width * 3U;
        const std::size_t targetRow = static_cast<std::size_t>(row) * sideBySide.width * 3U;
        std::copy_n(reference.pixels.data() + sourceRow, reference.width * 3U,
                    sideBySide.pixels.data() + targetRow);
        std::copy_n(candidate.pixels.data() + sourceRow, reference.width * 3U,
                    sideBySide.pixels.data() + targetRow + reference.width * 3U);
        std::copy_n(heatmap.pixels.data() + sourceRow, reference.width * 3U,
                    sideBySide.pixels.data() + targetRow + reference.width * 6U);
    }

    const std::string prefix = argv[3];
    if (!WritePpm(prefix + "-heatmap.ppm", heatmap) || !WritePpm(prefix + "-sidebyside.ppm", sideBySide)) {
        std::fprintf(stderr, "unable to write outputs with prefix %s\n", prefix.c_str());
        return 2;
    }
    std::printf("%s vs %s: mean_channel_difference=%.6f mismatch_fraction=%.6f\n", argv[1], argv[2],
                accumulator.MeanChannelDifference(), accumulator.MismatchFraction());
    return 0;
}
