#import <OpenGL/gl3.h>

#include "OpenGLFrameRenderer.h"

#include "OpenGLBloom.h"
#include "OpenGLParticleSystem.h"
#include "OpenGLRenderTargets.h"
#include "OpenGLSevenSegmentFps.h"
#include "OpenGLStarField.h"
#include "OpenGLToneMapper.h"

#include <algorithm>

namespace ParticleSaturn::Gpu::OpenGL41 {

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
    return starPass() && simulationPass() && particlePass() && bloomPass() && toneMapPass() &&
           capturePass() && acrylicPass() && presentPass() && fpsPass() && uiPass() && swapPass();
}

} // namespace ParticleSaturn::Gpu::OpenGL41
