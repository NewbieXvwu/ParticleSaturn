#include "SIMDNormalizeDispatch.h"

#include <initializer_list>

namespace SIMDNormalize::Internal {

namespace {

bool SupportsX86Flip(KernelImplementation implementation, const CpuFeatureSet& features) {
    switch (implementation) {
    case KernelImplementation::SSSE3:
#if PARTICLESATURN_SIMD_HAS_SSSE3_KERNEL
        return features.ssse3;
#else
        return false;
#endif
    case KernelImplementation::SSE41:
#if PARTICLESATURN_SIMD_HAS_SSE41_KERNEL
        return features.sse41;
#else
        return false;
#endif
    case KernelImplementation::AVX2:
#if PARTICLESATURN_SIMD_HAS_AVX2_KERNEL
        return features.avx2;
#else
        return false;
#endif
    default:
        return false;
    }
}

} // namespace

bool KernelRegistry::Supports(KernelImplementation implementation, KernelOperation operation) const {
    switch (implementation) {
    case KernelImplementation::Scalar:
        return true;
    case KernelImplementation::SSE2:
#if PARTICLESATURN_SIMD_HAS_SSE2_KERNEL
        return features_.sse2 && operation == KernelOperation::Normalize;
#else
        return false;
#endif
    case KernelImplementation::SSSE3:
    case KernelImplementation::SSE41:
    case KernelImplementation::AVX2:
        if (operation == KernelOperation::Normalize) {
            return SupportsX86Flip(implementation, features_);
        }
        return SupportsX86Flip(implementation, features_);
    case KernelImplementation::NEON:
#if PARTICLESATURN_SIMD_HAS_NEON_KERNEL
        return features_.neon && operation == KernelOperation::Normalize;
#else
        return false;
#endif
    case KernelImplementation::AVX512:
    case KernelImplementation::FMA:
    case KernelImplementation::DotProd:
    case KernelImplementation::I8MM:
        return false;
    }
    return false;
}

KernelImplementation KernelDispatcher::Select(SIMDMode mode, KernelOperation operation, CpuFeatureSet features) {
    const KernelRegistry registry{features};
    const auto           supports = [&registry, operation](KernelImplementation implementation) {
        return registry.Supports(implementation, operation);
    };
    const auto selectAutomatic = [&supports] {
        for (const auto implementation :
             {KernelImplementation::NEON, KernelImplementation::AVX2, KernelImplementation::SSE41,
              KernelImplementation::SSSE3, KernelImplementation::SSE2}) {
            if (supports(implementation)) {
                return implementation;
            }
        }
        return KernelImplementation::Scalar;
    };

    switch (mode) {
    case SIMDMode::Scalar:
        return KernelImplementation::Scalar;
    case SIMDMode::NEON:
        return supports(KernelImplementation::NEON) ? KernelImplementation::NEON : KernelImplementation::Scalar;
    case SIMDMode::AVX2:
        if (supports(KernelImplementation::AVX2)) {
            return KernelImplementation::AVX2;
        }
        if (supports(KernelImplementation::SSE41)) {
            return KernelImplementation::SSE41;
        }
        if (supports(KernelImplementation::SSSE3)) {
            return KernelImplementation::SSSE3;
        }
        if (supports(KernelImplementation::SSE2)) {
            return KernelImplementation::SSE2;
        }
        return KernelImplementation::Scalar;
    case SIMDMode::SSE41:
        if (supports(KernelImplementation::SSE41)) {
            return KernelImplementation::SSE41;
        }
        if (supports(KernelImplementation::SSSE3)) {
            return KernelImplementation::SSSE3;
        }
        if (supports(KernelImplementation::SSE2)) {
            return KernelImplementation::SSE2;
        }
        return KernelImplementation::Scalar;
    case SIMDMode::SSE:
        if (supports(KernelImplementation::SSE41)) {
            return KernelImplementation::SSE41;
        }
        if (supports(KernelImplementation::SSSE3)) {
            return KernelImplementation::SSSE3;
        }
        if (supports(KernelImplementation::SSE2)) {
            return KernelImplementation::SSE2;
        }
        return KernelImplementation::Scalar;
    case SIMDMode::Auto:
        return selectAutomatic();
    }
    return KernelImplementation::Scalar;
}

} // namespace SIMDNormalize::Internal
