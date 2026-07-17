// SIMDNormalize.cpp - 面向调用方的归一化接口和运行时分派

#include "CpuFeatureDetector.h"
#include "SIMDNormalizeDispatch.h"
#include "SIMDNormalizeKernels.h"

#include <iostream>

namespace SIMDNormalize {
namespace {

bool g_initialized = false;
CpuFeatureSet g_features{};
SIMDMode g_currentMode = SIMDMode::Auto;

Internal::KernelImplementation Select(Internal::KernelOperation operation) {
    return Internal::KernelDispatcher::Select(g_currentMode, operation, g_features);
}

const char* ImplementationName(Internal::KernelImplementation implementation, bool automatic) {
    switch (implementation) {
    case Internal::KernelImplementation::NEON: return automatic ? "NEON (auto)" : "NEON (forced)";
    case Internal::KernelImplementation::AVX2: return automatic ? "AVX2 (auto)" : "AVX2 (forced)";
    case Internal::KernelImplementation::SSE41: return automatic ? "SSE4.1 (auto)" : "SSE4.1 (forced)";
    case Internal::KernelImplementation::SSSE3: return automatic ? "SSSE3 (auto)" : "SSSE3 (forced)";
    case Internal::KernelImplementation::SSE2: return automatic ? "SSE2 (auto)" : "SSE2 (forced)";
    case Internal::KernelImplementation::Scalar: return automatic ? "Scalar (auto)" : "Scalar (forced fallback)";
    case Internal::KernelImplementation::AVX512:
    case Internal::KernelImplementation::FMA:
    case Internal::KernelImplementation::DotProd:
    case Internal::KernelImplementation::I8MM:
        return "Scalar (forced fallback)";
    }
    return "Scalar (forced fallback)";
}

bool IsModeSupported(SIMDMode mode) {
    switch (mode) {
    case SIMDMode::Auto:
    case SIMDMode::Scalar:
        return true;
    case SIMDMode::NEON:
        return g_features.neon &&
               Internal::KernelDispatcher::Select(mode, Internal::KernelOperation::Normalize, g_features) !=
                   Internal::KernelImplementation::Scalar;
    case SIMDMode::AVX2:
        return g_features.avx2 &&
               Internal::KernelDispatcher::Select(mode, Internal::KernelOperation::Normalize, g_features) !=
                   Internal::KernelImplementation::Scalar;
    case SIMDMode::SSE41:
        return g_features.sse41 &&
               Internal::KernelDispatcher::Select(mode, Internal::KernelOperation::Normalize, g_features) !=
                   Internal::KernelImplementation::Scalar;
    case SIMDMode::SSE:
        return g_features.sse2 &&
               Internal::KernelDispatcher::Select(mode, Internal::KernelOperation::Normalize, g_features) !=
                   Internal::KernelImplementation::Scalar;
    }
    return false;
}

} // namespace

void Init() {
    if (g_initialized) return;
    g_features = Internal::CpuFeatureDetector::Detect();
    g_initialized = true;
    std::cout << "[SIMD] CPU features detected - NEON: " << (g_features.neon ? "Yes" : "No")
              << ", AVX2: " << (g_features.avx2 ? "Yes" : "No")
              << ", SSE4.1: " << (g_features.sse41 ? "Yes" : "No")
              << ", SSSE3: " << (g_features.ssse3 ? "Yes" : "No")
              << ", SSE2: " << (g_features.sse2 ? "Yes" : "No") << std::endl;
}

CpuFeatureSet GetCpuFeatures() {
    if (!g_initialized) Init();
    return g_features;
}

void SetMode(SIMDMode mode) {
    if (!g_initialized) Init();
    if (!IsModeSupported(mode)) {
        std::cout << "[SIMD] Requested mode is unavailable; keeping " << GetCurrentImplementation() << std::endl;
        return;
    }
    g_currentMode = mode;
    std::cout << "[SIMD] Mode set to: " << GetCurrentImplementation() << std::endl;
}

SIMDMode GetMode() { return g_currentMode; }

bool IsAVX2Supported() { return GetCpuFeatures().avx2; }
bool IsSSE2Supported() { return GetCpuFeatures().sse2; }
bool IsSSE41Supported() { return GetCpuFeatures().sse41; }
bool IsNEONSupported() { return GetCpuFeatures().neon; }

const char* GetCurrentImplementation() {
    if (!g_initialized) Init();
    return ImplementationName(Select(Internal::KernelOperation::Normalize), g_currentMode == SIMDMode::Auto);
}

void NormalizeRGB(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    if (!g_initialized) Init();
    switch (Select(Internal::KernelOperation::Normalize)) {
    case Internal::KernelImplementation::NEON:
#if PARTICLESATURN_SIMD_HAS_NEON_KERNEL
        Kernels::NormalizeRGBNeon(src, dst, pixelCount);
        return;
#endif
        break;
    case Internal::KernelImplementation::AVX2:
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
        Kernels::NormalizeRGBAvx2(src, dst, pixelCount);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSE41:
#if PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
        Kernels::NormalizeRGBSse41(src, dst, pixelCount);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSSE3:
#if PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
        Kernels::NormalizeRGBSsse3(src, dst, pixelCount);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSE2:
#if PARTICLESATURN_SIMD_HAS_SSE2_KERNEL
        Kernels::NormalizeRGBSse2(src, dst, pixelCount);
        return;
#endif
        break;
    default:
        break;
    }
    Kernels::NormalizeRGBScalar(src, dst, pixelCount);
}

void NormalizeRGBRow(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    NormalizeRGB(src, dst, pixelCount);
}

void FlipHorizontalAndNormalize(const std::uint8_t* src, float* dst, int width, int height) {
    if (!g_initialized) Init();
    switch (Select(Internal::KernelOperation::FlipNormalize)) {
    case Internal::KernelImplementation::AVX2:
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
        Kernels::FlipHorizontalAndNormalizeAvx2(src, dst, width, height);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSE41:
#if PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
        Kernels::FlipHorizontalAndNormalizeSse41(src, dst, width, height);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSSE3:
#if PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
        Kernels::FlipHorizontalAndNormalizeSsse3(src, dst, width, height);
        return;
#endif
        break;
    default:
        break;
    }
    Kernels::FlipHorizontalAndNormalizeScalar(src, dst, width, height);
}

void FlipHorizontalAndBGR2RGB(const std::uint8_t* src, std::uint8_t* dst, int width, int height) {
    if (!g_initialized) Init();
    switch (Select(Internal::KernelOperation::FlipBgrToRgb)) {
    case Internal::KernelImplementation::AVX2:
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
        Kernels::FlipHorizontalAndBGR2RGBAvx2(src, dst, width, height);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSE41:
#if PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
        Kernels::FlipHorizontalAndBGR2RGBSse41(src, dst, width, height);
        return;
#endif
        break;
    case Internal::KernelImplementation::SSSE3:
#if PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
        Kernels::FlipHorizontalAndBGR2RGBSsse3(src, dst, width, height);
        return;
#endif
        break;
    default:
        break;
    }
    Kernels::FlipHorizontalAndBGR2RGBScalar(src, dst, width, height);
}

} // namespace SIMDNormalize
