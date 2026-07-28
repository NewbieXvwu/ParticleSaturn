#pragma once

#include "SIMDNormalize.h"

namespace SIMDNormalize::Internal {

enum class KernelOperation {
    Normalize,
    FlipNormalize,
    FlipBgrToRgb
};
enum class KernelImplementation {
    Scalar,
    SSE2,
    SSSE3,
    SSE41,
    AVX2,
    NEON,
    // 预留扩展点：当前项目没有对应内核。
    AVX512,
    FMA,
    DotProd,
    I8MM,
};

class KernelRegistry {
  public:
    explicit KernelRegistry(CpuFeatureSet features) : features_{features} {}

    bool Supports(KernelImplementation implementation, KernelOperation operation) const;

  private:
    CpuFeatureSet features_;
};

class KernelDispatcher {
  public:
    static KernelImplementation Select(SIMDMode mode, KernelOperation operation, CpuFeatureSet features);
};

} // namespace SIMDNormalize::Internal
