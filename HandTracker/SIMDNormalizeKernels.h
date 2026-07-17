#pragma once

#include <cstddef>
#include <cstdint>

namespace SIMDNormalize::Kernels {

void NormalizeRGBScalar(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void FlipHorizontalAndNormalizeScalar(const std::uint8_t* src, float* dst, int width, int height);
void FlipHorizontalAndBGR2RGBScalar(const std::uint8_t* src, std::uint8_t* dst, int width, int height);

void NormalizeRGBNeon(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void NormalizeRGBSse2(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void NormalizeRGBSsse3(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void FlipHorizontalAndNormalizeSsse3(const std::uint8_t* src, float* dst, int width, int height);
void FlipHorizontalAndBGR2RGBSsse3(const std::uint8_t* src, std::uint8_t* dst, int width, int height);

void NormalizeRGBSse41(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void FlipHorizontalAndNormalizeSse41(const std::uint8_t* src, float* dst, int width, int height);
void FlipHorizontalAndBGR2RGBSse41(const std::uint8_t* src, std::uint8_t* dst, int width, int height);

void NormalizeRGBAvx2(const std::uint8_t* src, float* dst, std::size_t pixelCount);
void FlipHorizontalAndNormalizeAvx2(const std::uint8_t* src, float* dst, int width, int height);
void FlipHorizontalAndBGR2RGBAvx2(const std::uint8_t* src, std::uint8_t* dst, int width, int height);

} // namespace SIMDNormalize::Kernels
