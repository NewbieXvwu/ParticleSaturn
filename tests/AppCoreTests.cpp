#include "app/AppController.h"
#include "app/FrameCoordinator.h"

#include <cassert>
#include <cmath>

using namespace ParticleSaturn::App;

int main() {
    AppController controller;

    const auto particles = controller.Dispatch(SetParticleCount{1});
    assert(particles.renderSettingsChanged);
    assert(controller.State().render.particleCount == RenderSettings::MinParticles);

    const auto api = controller.Dispatch(SetGraphicsApi{GraphicsApi::Metal});
    assert(api.restartRequired);
    assert(controller.State().render.graphicsApi == GraphicsApi::Metal);

    const auto driver = controller.Dispatch(SetVulkanDriver{VulkanDriver::KosmicKrisp});
    assert(driver.restartRequired);

    controller.Dispatch(SetBlurStrength{99.0f});
    assert(controller.State().ui.blurStrength == 5.0f);
    assert(controller.State().render.bloomBlurStrength == 2.0f);

    FrameCoordinator coordinator{0.01};
    const auto frame = coordinator.Advance(controller, 0.025, {true, 2.0f, -1.0f, 0.5f});
    assert(frame.frameIndex == 1);
    assert(frame.state == &controller.State());
    assert(std::abs(controller.State().scene.simulationTimeSeconds - 0.02) < 0.0001);
    assert(std::abs(controller.State().scene.rotationX - 4.4f) < 0.0001f);
    assert(std::abs(controller.State().scene.rotationY + 2.0f) < 0.0001f);

    AppController diligentController;
    FrameCoordinator diligentCoordinator{1.0 / 180.0};
    diligentCoordinator.Advance(diligentController, 1.0 / 180.0);
    const auto& automatic = diligentController.State().scene;
    const float automaticAlpha = 0.08f;
    const float automaticTime = 0.005f;
    assert(std::abs(automatic.autoAnimationTime - automaticTime) < 0.0001f);
    assert(std::abs(automatic.zoom - (1.0f + std::sin(automaticTime) * 0.2f * automaticAlpha)) < 0.0001f);

    AppController handController;
    FrameCoordinator handCoordinator{1.0 / 180.0};
    handCoordinator.Advance(handController, 1.0 / 180.0, {true, 0.0f, 0.0f, 0.0f, true, 1.5f, 0.25f, 0.75f});
    const auto& hand = handController.State().scene;
    assert(std::abs(hand.zoom - 1.125f) < 0.0001f);
    assert(std::abs(hand.rotationX - 0.45f) < 0.0001f);
    assert(std::abs(hand.rotationY + 0.125f) < 0.0001f);

    controller.Dispatch(TogglePause{});
    coordinator.Advance(controller, 0.01, {true, 1.0f, 1.0f, 1.0f});
    assert(std::abs(controller.State().scene.simulationTimeSeconds - 0.02) < 0.0001);

    AppController lodController;
    FrameCoordinator lodCoordinator{1.0 / 120.0};
    const auto initialParticles = lodController.State().render.particleCount;
    for (int frame = 0; frame < 16; ++frame) lodCoordinator.Advance(lodController, 0.05);
    assert(lodController.State().render.particleCount < initialParticles);
    lodController.Dispatch(SetLodLocked{true});
    const auto lockedParticles = lodController.State().render.particleCount;
    for (int frame = 0; frame < 16; ++frame) lodCoordinator.Advance(lodController, 0.05);
    assert(lodController.State().render.particleCount == lockedParticles);
    lodController.Dispatch(SetLodLocked{false});
    lodController.MutableState().render.particleCount = RenderSettings::MinParticles;
    lodController.MutableState().render.pixelRatio = 1.0f;
    lodController.MutableState().lod.smoothedFrameSeconds = 1.0f / 240.0f;
    lodCoordinator.Advance(lodController, 1.0 / 240.0);
    assert(lodController.State().render.particleCount > RenderSettings::MinParticles);
    return 0;
}
