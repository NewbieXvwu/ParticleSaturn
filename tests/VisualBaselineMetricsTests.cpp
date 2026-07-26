#include "common/ImageMetrics.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
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

} // namespace

int main(int argc, char* argv[]) {
    assert(argc >= 5);
    const float meanLimit = std::stof(argv[argc - 2]);
    const float mismatchLimit = std::stof(argv[argc - 1]);
    Image reference;
    assert(ReadPpm(argv[1], reference));
    for (int imageIndex = 2; imageIndex < argc - 2; ++imageIndex) {
        Image candidate;
        assert(ReadPpm(argv[imageIndex], candidate));
        assert(reference.width == candidate.width && reference.height == candidate.height);
        ParticleSaturn::TestCommon::ImageDifferenceAccumulator accumulator;
        for (std::size_t pixel = 0; pixel < reference.pixels.size(); pixel += 3U) {
            accumulator.AddPixel(reference.pixels[pixel], reference.pixels[pixel + 1U], reference.pixels[pixel + 2U],
                                 candidate.pixels[pixel], candidate.pixels[pixel + 1U], candidate.pixels[pixel + 2U]);
        }
        const float meanDifference = static_cast<float>(accumulator.MeanChannelDifference());
        const float mismatchFraction = static_cast<float>(accumulator.MismatchFraction());
        std::cout << argv[imageIndex] << ": mean_channel_difference=" << meanDifference
                  << " mismatch_fraction=" << mismatchFraction << '\n';
        assert(meanDifference <= meanLimit);
        assert(mismatchFraction <= mismatchLimit);
    }
    return 0;
}
