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

    FrameCoordinator coordinator{0.01};
    const auto frame = coordinator.Advance(controller, 0.025, {true, 2.0f, -1.0f, 0.5f});
    assert(frame.frameIndex == 1);
    assert(frame.state == &controller.State());
    assert(std::abs(controller.State().scene.simulationTimeSeconds - 0.02) < 0.0001);
    assert(std::abs(controller.State().scene.rotationX - 4.0f) < 0.0001f);
    assert(std::abs(controller.State().scene.rotationY + 2.0f) < 0.0001f);

    controller.Dispatch(TogglePause{});
    coordinator.Advance(controller, 0.01, {true, 1.0f, 1.0f, 1.0f});
    assert(std::abs(controller.State().scene.simulationTimeSeconds - 0.02) < 0.0001);
    return 0;
}
