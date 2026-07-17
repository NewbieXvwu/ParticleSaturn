#import <Cocoa/Cocoa.h>

#include "CocoaHost.h"
#include "MacOSApplication.h"
#include "app/AppController.h"
#include "gpu/backends/diligent/DiligentVulkanAdapter.h"
#include "services/diagnostics/DiagnosticBus.h"
#include "services/settings/macos/NSUserDefaultsStore.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

ParticleSaturn::App::VulkanDriver SelectedDriver(ParticleSaturn::App::VulkanDriver saved) {
    const char* overrideValue = std::getenv("PARTICLESATURN_VULKAN_DRIVER");
    if (overrideValue == nullptr || overrideValue[0] == '\0') return saved;
    const std::string value{overrideValue};
    if (value == "molten" || value == "moltenvk") return ParticleSaturn::App::VulkanDriver::MoltenVK;
    if (value == "kosmic" || value == "kosmickrisp") return ParticleSaturn::App::VulkanDriver::KosmicKrisp;
    return saved;
}

std::uint32_t SmokeFrameLimit() {
    const char* value = std::getenv("PARTICLESATURN_VULKAN_SMOKE_FRAMES");
    if (value == nullptr || value[0] == '\0') return 0;
    const auto parsed = std::strtoul(value, nullptr, 10);
    return static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 10000UL));
}

int ReportStartupFailure(const char* code, const std::string& message) {
    ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Publish(
        "vulkan", code, message, ParticleSaturn::Services::Diagnostics::Severity::Error);
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int ParticleSaturn::Platform::MacOS::RunVulkanApplication() {
    @autoreleasepool {
        Services::Settings::MacOS::NSUserDefaultsStore settings;
        App::AppState defaults;
        defaults.render.graphicsApi = App::GraphicsApi::Vulkan;
        auto state = settings.Load(defaults);
        state.render.graphicsApi = App::GraphicsApi::Vulkan;
        state.render.vulkanDriver = SelectedDriver(state.render.vulkanDriver);

        CocoaHost host{state.window.width, state.window.height, "Particle Saturn - Vulkan"};
        host.SetWindowPosition(state.window.x, state.window.y);
        host.SetPresentationMode(state.render.vsyncMode);
        host.SetWindowMaterial(state.window.material);

        const char* resourcesPath = [[[NSBundle mainBundle] resourcePath] fileSystemRepresentation];
        if (resourcesPath == nullptr) return ReportStartupFailure("resources", "Vulkan bundle resources are unavailable");
        Gpu::Diligent::DiligentVulkanAdapter adapter;
        std::string error;
        if (!adapter.Initialize(state.render.vulkanDriver, resourcesPath, error)) {
            return ReportStartupFailure("device", error);
        }
        auto drawableSize = host.CurrentDrawableSize();
        if (!adapter.CreateSwapChain(host.NativeView(), drawableSize.width, drawableSize.height, error)) {
            return ReportStartupFailure("swap-chain", error);
        }

        const auto smokeFrames = SmokeFrameLimit();
        const bool smokeMode = smokeFrames != 0;
        App::AppController controller{state};
        host.SetActionCallback([&](HostAction action) {
            switch (action) {
            case HostAction::ToggleFullscreen: {
                const auto effect = controller.Dispatch(App::SetFullscreen{!controller.State().window.fullscreen});
                if (effect.windowChanged) host.ToggleFullscreen();
                break;
            }
            case HostAction::KeyF11Down:
            case HostAction::KeyF11Up:
            case HostAction::KeyEscapeDown:
            case HostAction::KeyEscapeUp: {
                const bool pressed = action == HostAction::KeyF11Down || action == HostAction::KeyEscapeDown;
                const auto key = action == HostAction::KeyF11Down || action == HostAction::KeyF11Up
                    ? App::InputKey::F11 : App::InputKey::Escape;
                const auto effect = controller.Dispatch(App::SetInputKeyPressed{key, pressed});
                if (effect.windowChanged) host.ToggleFullscreen();
                if (effect.exitRequested) host.RequestExit();
                break;
            }
            default:
                break;
            }
            if (!smokeMode) settings.Save(controller.State());
        });

        std::uint32_t renderedFrames = 0;
        auto appliedVsync = state.render.vsyncMode;
        host.Show();
        host.Run([&] {
            const auto currentSize = host.CurrentDrawableSize();
            if (currentSize.width != drawableSize.width || currentSize.height != drawableSize.height) {
                if (!adapter.ResizeSwapChain(currentSize.width, currentSize.height)) {
                    ReportStartupFailure("resize", "Vulkan swap chain resize failed");
                    host.RequestExit();
                    return;
                }
                drawableSize = currentSize;
            }
            auto& mutableState = controller.MutableState();
            mutableState.window.width = static_cast<std::uint32_t>(drawableSize.width / drawableSize.scale);
            mutableState.window.height = static_cast<std::uint32_t>(drawableSize.height / drawableSize.scale);
            mutableState.window.dpiScale = drawableSize.scale;
            if (!mutableState.window.fullscreen) {
                host.GetWindowPosition(mutableState.window.x, mutableState.window.y);
                mutableState.window.windowedX = mutableState.window.x;
                mutableState.window.windowedY = mutableState.window.y;
                mutableState.window.windowedWidth = mutableState.window.width;
                mutableState.window.windowedHeight = mutableState.window.height;
            }
            if (mutableState.render.vsyncMode != appliedVsync) {
                host.SetPresentationMode(mutableState.render.vsyncMode);
                appliedVsync = mutableState.render.vsyncMode;
            }
            constexpr float clearColor[] = {0.002f, 0.003f, 0.008f, 1.0f};
            const auto syncInterval = mutableState.render.vsyncMode == 0 ? 0U : 1U;
            if (!adapter.PresentClearFrame(clearColor, syncInterval)) {
                ReportStartupFailure("present", "Vulkan frame presentation failed");
                host.RequestExit();
                return;
            }
            if (smokeFrames != 0 && ++renderedFrames >= smokeFrames) host.RequestExit();
        });
        if (!smokeMode) settings.Save(controller.State());
        adapter.Shutdown();
    }
    return 0;
}
