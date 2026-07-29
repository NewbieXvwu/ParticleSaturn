#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace ParticleSaturn::App {

// 三条路径共用的 FPS 度量（D-001：测量方法只有一份）。原先各平台外壳各持一份
// 相同实现（macOS AppShell.mm 的匿名 FpsMeter、Diligent 后端内联的帧时环形均值），
// 现上提为平台中立单一定义，macOS 与 Windows 外壳共用。
class FpsMeter {
public:
    void AddSample(float deltaTime) {
        if (deltaTime <= 0.0f || deltaTime >= 1.0f) return;
        samples_[next_] = deltaTime;
        next_ = (next_ + 1U) % samples_.size();
        float total = 0.0f;
        for (const float sample : samples_) total += sample;
        framesPerSecond_ = total > 0.0f ? static_cast<float>(samples_.size()) / total : 60.0f;
    }

    std::uint32_t Value() const {
        return static_cast<std::uint32_t>(std::clamp(framesPerSecond_, 0.0f, 999.0f));
    }

    float ValueFloat() const { return framesPerSecond_; }

private:
    std::array<float, 60> samples_{};
    std::size_t next_ = 0;
    float framesPerSecond_ = 60.0f;
};

} // namespace ParticleSaturn::App
