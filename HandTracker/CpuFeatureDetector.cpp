#include "CpuFeatureDetector.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace SIMDNormalize::Internal {

namespace {

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
void GetCPUID(int info[4], int functionId) {
#if defined(_MSC_VER)
    __cpuid(info, functionId);
#else
    __cpuid(functionId, info[0], info[1], info[2], info[3]);
#endif
}

void GetCPUIDEx(int info[4], int functionId, int subfunctionId) {
#if defined(_MSC_VER)
    __cpuidex(info, functionId, subfunctionId);
#else
    __cpuid_count(functionId, subfunctionId, info[0], info[1], info[2], info[3]);
#endif
}

std::uint64_t GetXCR0() {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32U) | low;
#endif
}
#endif

#if defined(__APPLE__)
bool ReadSysctlFlag(const char* name) {
    int value = 0;
    std::size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 && value != 0;
}
#endif

} // namespace

CpuFeatureSet CpuFeatureDetector::Detect() {
    CpuFeatureSet features{};

#if defined(_M_X64) || defined(_M_IX86) || defined(__i386__) || defined(__x86_64__)
    int info[4]{};
    GetCPUID(info, 0);
    const int maxFunction = info[0];
    bool osAvxState = false;
    bool osAvx512State = false;
    if (maxFunction >= 1) {
        GetCPUID(info, 1);
        features.sse2 = (info[3] & (1 << 26)) != 0;
        features.ssse3 = (info[2] & (1 << 9)) != 0;
        features.sse41 = (info[2] & (1 << 19)) != 0;
        const bool osxsave = (info[2] & (1 << 27)) != 0;
        const bool avx = (info[2] & (1 << 28)) != 0;
        if (osxsave && avx) {
            const auto xcr0 = GetXCR0();
            osAvxState = (xcr0 & 0x6U) == 0x6U;
            osAvx512State = (xcr0 & 0xE6U) == 0xE6U;
            features.fma = osAvxState && (info[2] & (1 << 12)) != 0;
        }
    }
    if (maxFunction >= 7) {
        GetCPUIDEx(info, 7, 0);
        features.avx2 = osAvxState && (info[1] & (1 << 5)) != 0;
        features.avx512 = osAvx512State && (info[1] & (1 << 16)) != 0;
    }
#endif

#if defined(__aarch64__) || defined(__arm64__)
#if defined(__APPLE__)
    features.neon = ReadSysctlFlag("hw.optional.neon");
    features.dotProd = ReadSysctlFlag("hw.optional.arm.FEAT_DotProd");
    features.i8mm = ReadSysctlFlag("hw.optional.arm.FEAT_I8MM");
#else
    features.neon = true;
#endif
#endif
    return features;
}

} // namespace SIMDNormalize::Internal
