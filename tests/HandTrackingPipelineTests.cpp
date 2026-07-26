#include "app/FrameCoordinator.h"
#include "services/hand_tracking/macos/HandTrackingWorker.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"

#include <cassert>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using namespace ParticleSaturn;

// 端到端流水线：真实 XNNPACK 运行时 + Main.mm 中的生产处理器
// （Invoke && DecodeLandmarks）→ HandTrackingWorker 后台线程 →
// AppController/FrameCoordinator。校验一帧无手画面经完整链路后得到
// “未跟踪”手势，并触发自动动画（与 AppCoreTests 的 Diligent 参照一致）。
int main() {
    Services::HandTracking::MacOS::XnnpackHandTrackingRuntime runtime;
    std::string loadError;
    assert(runtime.Load(PARTICLESATURN_PALM_MODEL_PATH, PARTICLESATURN_LANDMARK_MODEL_PATH, loadError));
    assert(runtime.IsLoaded());

    std::mutex mutex;
    std::condition_variable processed;
    int processedCount = 0;

    Services::HandTracking::MacOS::HandTrackingWorker worker{
        [&](const Services::Camera::Frame& frame, Services::HandTracking::MacOS::HandPose& pose,
            std::string& error) {
            // 与 src/platform/macos/Main.mm 生产处理器逐字一致的组合调用。
            const bool tracked = runtime.Invoke(frame, error) && runtime.DecodeLandmarks(pose);
            {
                std::lock_guard lock{mutex};
                ++processedCount;
            }
            processed.notify_one();
            return tracked;
        }};

    Services::Camera::Frame blankFrame{64, 48, 0, 64U * 3U, Services::Camera::PixelFormat::RGB24,
                                       Services::Camera::FrameOrientation::Up, false,
                                       std::vector<std::uint8_t>(64U * 48U * 3U, 127U)};
    worker.Submit(std::move(blankFrame), 2);
    worker.Start();
    {
        std::unique_lock lock{mutex};
        assert(processed.wait_for(lock, std::chrono::seconds{5}, [&] { return processedCount >= 1; }));
    }

    const auto gesture = worker.LatestGesture();
    assert(!gesture.tracked);

    App::AppController controller;
    App::FrameCoordinator coordinator{1.0 / 180.0};
    coordinator.Advance(controller, 1.0 / 180.0, gesture);
    const auto& scene = controller.State().scene;
    const float automaticAlpha = 0.08f;
    const float automaticTime = 0.005f;
    assert(std::abs(scene.autoAnimationTime - automaticTime) < 0.0001f);
    assert(std::abs(scene.zoom - (1.0f + std::sin(automaticTime) * 0.2f * automaticAlpha)) < 0.0001f);

    worker.Stop();
    return 0;
}
