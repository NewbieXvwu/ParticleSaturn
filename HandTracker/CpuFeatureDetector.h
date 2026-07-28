#pragma once

#include "SIMDNormalize.h"

namespace SIMDNormalize::Internal {

class CpuFeatureDetector {
  public:
    static CpuFeatureSet Detect();
};

} // namespace SIMDNormalize::Internal
