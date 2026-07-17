#include "SIMDNormalizeKernels.h"

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

namespace SIMDNormalize::Kernels {

void NormalizeRGBNeon(const std::uint8_t* src, float* dst, std::size_t pixelCount) {
    constexpr float scale = 1.0f / 255.0f;
    const std::size_t channelCount = pixelCount * 3;
    std::size_t index = 0;
    for (; index + 16 <= channelCount; index += 16) {
        const uint8x16_t bytes = vld1q_u8(src + index);
        const uint16x8_t low16 = vmovl_u8(vget_low_u8(bytes));
        const uint16x8_t high16 = vmovl_u8(vget_high_u8(bytes));
        vst1q_f32(dst + index, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(low16))), scale));
        vst1q_f32(dst + index + 4, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(low16))), scale));
        vst1q_f32(dst + index + 8, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(high16))), scale));
        vst1q_f32(dst + index + 12, vmulq_n_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(high16))), scale));
    }
    for (; index < channelCount; ++index) dst[index] = src[index] * scale;
}

} // namespace SIMDNormalize::Kernels
#endif
