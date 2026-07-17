#import <OpenGL/gl3.h>

#include "OpenGLFrameRenderer.h"

#include "OpenGLBloom.h"
#include "OpenGLParticleSystem.h"
#include "OpenGLRenderTargets.h"
#include "OpenGLSevenSegmentFps.h"
#include "OpenGLStarField.h"
#include "OpenGLToneMapper.h"
#include "render/RenderGraph.h"

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

    Render::RenderGraph graph;
    const auto particleState = graph.AddResource({"particle-state", {1, 1, 1}});
    const auto sceneHandle = targets.SceneHandle();
    const auto bloomStrongHandle = targets.BloomStrongHandle();
    const auto bloomPingPongHandle = targets.BloomPingPongHandle();
    const auto bloomWeakHandle = targets.BloomWeakHandle();
    const auto bloomWeakPingPongHandle = targets.BloomWeakPingPongHandle();
    const auto toneMappedHandle = targets.ToneMappedHandle();
    if (!sceneHandle || !bloomStrongHandle || !bloomPingPongHandle || !bloomWeakHandle ||
        !bloomWeakPingPongHandle || !toneMappedHandle) return false;
    const auto scene = graph.AddResource({"scene-hdr", {width, height, 1}, sceneHandle});
    const auto strongWidth = std::max(1U, width / 6U);
    const auto strongHeight = std::max(1U, height / 6U);
    const auto weakWidth = std::max(1U, width / 12U);
    const auto weakHeight = std::max(1U, height / 12U);
    const auto bloomStrong = graph.AddResource({"bloom-strong", {strongWidth, strongHeight, 1}, bloomStrongHandle});
    const auto bloomPingPong = graph.AddResource({"bloom-ping-pong", {strongWidth, strongHeight, 1}, bloomPingPongHandle});
    const auto bloomWeak = graph.AddResource({"bloom-weak", {weakWidth, weakHeight, 1}, bloomWeakHandle});
    const auto bloomWeakPingPong = graph.AddResource({"bloom-weak-ping-pong", {weakWidth, weakHeight, 1}, bloomWeakPingPongHandle});
    const auto toneMapped = graph.AddResource({"tone-mapped", {width, height, 1}, toneMappedHandle});
    const auto drawable = graph.AddResource({"drawable", {width, height, 1}});

    const auto starPass = graph.AddPass("starfield", [&] {
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
    });
    const auto simulationPass = graph.AddPass("particle-simulation", [&] {
        if (!state.scene.paused) particles.Simulate(deltaTime, state.scene.zoom, handTracked);
        return glGetError() == GL_NO_ERROR;
    });
    const auto particlePass = graph.AddPass("particle-render", [&] {
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
    });
    const auto bloomPass = graph.AddPass("bloom", [&] {
        return bloom.Apply(targets, state.render.bloomBlurStrength);
    });
    const auto toneMapPass = graph.AddPass("tone-map", [&] {
        return toneMapper.Apply(targets, state.render.bloomEnabled ? 0.5f : 0.0f, transparent);
    });
    const auto capturePass = graph.AddPass("capture", [&] {
        return !callbacks.capture || callbacks.capture(targets.NativeFramebuffer(toneMappedHandle), width, height);
    });
    const auto acrylicPass = graph.AddPass("ui-acrylic", [&] {
        return !state.ui.blurEnabled || bloom.ApplyUiBlur(targets, state.ui.blurStrength);
    });
    const auto presentPass = graph.AddPass("final-composite", [&] {
        return toneMapper.Present(targets, transparent);
    });
    const auto fpsPass = graph.AddPass("seven-segment", [&] {
        return sevenSegment.Render(0, width, height, framesPerSecond);
    });
    const auto uiPass = graph.AddPass("imgui", [&] {
        return !callbacks.renderUi || callbacks.renderUi(targets.NativeTexture(bloomStrongHandle),
                                                         targets.NativeTexture(bloomWeakHandle));
    });
    const auto swapPass = graph.AddPass("present", [&] {
        if (callbacks.present) callbacks.present();
        return true;
    });

    graph.Write(starPass, scene, ResourceUsage::RenderTarget);
    graph.Write(simulationPass, particleState, ResourceUsage::ShaderWrite);
    graph.Read(particlePass, particleState, ResourceUsage::ShaderRead);
    graph.Read(particlePass, scene, ResourceUsage::RenderTarget);
    graph.Write(particlePass, scene, ResourceUsage::RenderTarget);
    graph.Read(bloomPass, scene, ResourceUsage::ShaderRead);
    graph.Write(bloomPass, bloomStrong, ResourceUsage::RenderTarget);
    graph.Write(bloomPass, bloomPingPong, ResourceUsage::RenderTarget);
    graph.Write(bloomPass, bloomWeak, ResourceUsage::RenderTarget);
    graph.Write(bloomPass, bloomWeakPingPong, ResourceUsage::RenderTarget);
    graph.Read(toneMapPass, scene, ResourceUsage::ShaderRead);
    graph.Read(toneMapPass, bloomPingPong, ResourceUsage::ShaderRead);
    graph.Write(toneMapPass, toneMapped, ResourceUsage::RenderTarget);
    graph.Read(capturePass, toneMapped, ResourceUsage::CopySource);
    graph.Read(acrylicPass, toneMapped, ResourceUsage::ShaderRead);
    graph.Write(acrylicPass, bloomStrong, ResourceUsage::RenderTarget);
    graph.Write(acrylicPass, bloomPingPong, ResourceUsage::RenderTarget);
    graph.Write(acrylicPass, bloomWeak, ResourceUsage::RenderTarget);
    graph.Write(acrylicPass, bloomWeakPingPong, ResourceUsage::RenderTarget);
    graph.Read(presentPass, toneMapped, ResourceUsage::ShaderRead);
    graph.Write(presentPass, drawable, ResourceUsage::RenderTarget);
    graph.Read(fpsPass, drawable, ResourceUsage::RenderTarget);
    graph.Write(fpsPass, drawable, ResourceUsage::RenderTarget);
    graph.Read(uiPass, drawable, ResourceUsage::RenderTarget);
    graph.Read(uiPass, bloomStrong, ResourceUsage::ShaderRead);
    graph.Read(uiPass, bloomWeak, ResourceUsage::ShaderRead);
    graph.Write(uiPass, drawable, ResourceUsage::RenderTarget);
    graph.Read(swapPass, drawable, ResourceUsage::Present);
    return graph.Execute();
}

} // namespace ParticleSaturn::Gpu::OpenGL41
