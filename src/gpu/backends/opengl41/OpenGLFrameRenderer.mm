#import <OpenGL/gl3.h>

#include "OpenGLFrameRenderer.h"

#include "OpenGLBloom.h"
#include "OpenGLParticleSystem.h"
#include "OpenGLRenderTargets.h"
#include "OpenGLSevenSegmentFps.h"
#include "OpenGLStarField.h"
#include "OpenGLToneMapper.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace ParticleSaturn::Gpu::OpenGL41 {

namespace {

// 逐 pass 捕获（TODO P4，2026-07-27 拍板）：PARTICLESATURN_CAPTURE_PASS_DIR
// 设定时把中间目标写为 <dir>/<pass>.ppm。8-bit clamp、行序翻转到自上而下，
// 与 Metal/Vulkan 的捕获同规，供对比工具逐 pass 比对。
const char* PassCaptureDirectory() {
    static const char* directory = std::getenv("PARTICLESATURN_CAPTURE_PASS_DIR");
    return directory != nullptr && directory[0] != '\0' ? directory : nullptr;
}

void MaybeCapturePass(const char* passName, std::uint32_t framebuffer, std::uint32_t width, std::uint32_t height) {
    const char* directory = PassCaptureDirectory();
    if (directory == nullptr || width == 0 || height == 0) return;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (glGetError() != GL_NO_ERROR) return;
    std::ofstream output{std::string{directory} + "/" + passName + ".ppm", std::ios::binary};
    if (!output) return;
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::uint32_t sourceRow = height - 1U - row;
        for (std::uint32_t column = 0; column < width; ++column) {
            const auto* pixel = &pixels[(static_cast<std::size_t>(sourceRow) * width + column) * 4U];
            output.write(reinterpret_cast<const char*>(pixel), 3);
        }
    }
}

} // namespace

bool OpenGLFrameRenderer::Render(OpenGLParticleSystem& particles, OpenGLStarField& stars,
                                 OpenGLRenderTargets& targets, OpenGLBloom& bloom,
                                 OpenGLToneMapper& toneMapper, OpenGLSevenSegmentFps& sevenSegment,
                                 std::uint32_t width, std::uint32_t height, const App::AppState& state,
                                 bool handTracked, float deltaTime, std::uint32_t framesPerSecond,
                                 bool transparent, const OpenGLFrameCallbacks& callbacks) {
    if (width == 0 || height == 0) return false;
    if ((targets.Width() != width || targets.Height() != height) && !targets.Create(width, height)) return false;

    const auto sceneHandle = targets.SceneHandle();
    const auto bloomStrongHandle = targets.BloomStrongHandle();
    const auto bloomPingPongHandle = targets.BloomPingPongHandle();
    const auto bloomWeakHandle = targets.BloomWeakHandle();
    const auto bloomWeakPingPongHandle = targets.BloomWeakPingPongHandle();
    const auto toneMappedHandle = targets.ToneMappedHandle();
    if (!sceneHandle || !bloomStrongHandle || !bloomPingPongHandle || !bloomWeakHandle ||
        !bloomWeakPingPongHandle || !toneMappedHandle) return false;
    const auto strongWidth = std::max(1U, width / 6U);
    const auto strongHeight = std::max(1U, height / 6U);
    const auto weakWidth = std::max(1U, width / 12U);
    const auto weakHeight = std::max(1U, height / 12U);

    const auto starPass = [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glViewport(0, 0, width, height);
        glClearColor(0.002f, 0.003f, 0.008f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        stars.Draw(static_cast<float>(state.scene.simulationTimeSeconds), width, height);
        glDisable(GL_BLEND);
        return glGetError() == GL_NO_ERROR;
    };
    const auto simulationPass = [&] {
        if (!state.scene.paused) particles.Simulate(deltaTime, state.scene.zoom, handTracked);
        return glGetError() == GL_NO_ERROR;
    };
    const auto particlePass = [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, targets.SceneFramebuffer());
        glViewport(0, 0, width, height);
        glEnable(GL_BLEND);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        particles.DrawIndirect(static_cast<float>(state.scene.simulationTimeSeconds), width, height,
                               state.scene.zoom, state.scene.rotationX, state.scene.rotationY,
                               state.render.pixelRatio, state.render.densityCompensation,
                               state.render.particleCount);
        glDisable(GL_BLEND);
        return glGetError() == GL_NO_ERROR;
    };
    const auto bloomPass = [&] {
        return bloom.Apply(targets, state.render.bloomBlurStrength);
    };
    const auto toneMapPass = [&] {
        return toneMapper.Apply(targets, state.render.bloomEnabled ? 0.5f : 0.0f, transparent);
    };
    const auto capturePass = [&] {
        return !callbacks.capture || callbacks.capture(targets.NativeFramebuffer(toneMappedHandle), width, height);
    };
    const auto acrylicPass = [&] {
        return !state.ui.blurEnabled || bloom.ApplyUiBlur(targets, state.ui.blurStrength);
    };
    const auto presentPass = [&] {
        return toneMapper.Present(targets, transparent);
    };
    const auto fpsPass = [&] {
        return sevenSegment.Render(0, width, height, framesPerSecond);
    };
    const auto uiPass = [&] {
        return !callbacks.renderUi || callbacks.renderUi(targets.NativeTexture(bloomStrongHandle),
                                                         targets.NativeTexture(bloomWeakHandle));
    };
    const auto swapPass = [&] {
        if (callbacks.present) callbacks.present();
        return true;
    };

    // 通道按书写顺序静态直排（D-003）：原图 Compile 输出恒等于插入顺序。
    const auto particlePassWithCapture = [&] {
        if (!particlePass()) return false;
        MaybeCapturePass("scene-hdr", targets.SceneFramebuffer(), width, height);
        return true;
    };
    const auto bloomPassWithCapture = [&] {
        if (!bloomPass()) return false;
        MaybeCapturePass("bloom", targets.BloomPingPongFramebuffer(), std::max(1U, width / 6U),
                         std::max(1U, height / 6U));
        return true;
    };
    return starPass() && simulationPass() && particlePassWithCapture() && bloomPassWithCapture() && toneMapPass() &&
           capturePass() && acrylicPass() && presentPass() && fpsPass() && uiPass() && swapPass();
}

} // namespace ParticleSaturn::Gpu::OpenGL41
