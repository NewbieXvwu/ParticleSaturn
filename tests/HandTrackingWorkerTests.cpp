#include "services/hand_tracking/macos/HandTrackingWorker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

using namespace ParticleSaturn;

int main() {
    std::atomic<std::int64_t> elapsedMilliseconds{0};
    std::mutex mutex;
    std::condition_variable processed;
    int processedCount = 0;
    std::uint64_t lastTimestamp = 0;
    Services::HandTracking::MacOS::HandTrackingWorker worker{
        [&](const Services::Camera::Frame& frame, Services::HandTracking::MacOS::HandPose& pose, std::string&) {
            {
                std::lock_guard lock{mutex};
                lastTimestamp = frame.timestampNanoseconds;
                ++processedCount;
            }
            processed.notify_one();
            if (frame.timestampNanoseconds == 2U) {
                pose = {.confidence = 1.0f, .centerX = 0.25f, .centerY = 0.75f, .scale = 1.5f};
                return true;
            }
            return false;
        },
        [&] { return std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{elapsedMilliseconds.load()}; }};

    worker.Submit({.timestampNanoseconds = 1U}, 2);
    worker.Submit({.timestampNanoseconds = 2U}, 2);
    worker.Start();
    {
        std::unique_lock lock{mutex};
        assert(processed.wait_for(lock, std::chrono::seconds{1}, [&] { return processedCount == 1; }));
        assert(lastTimestamp == 2U);
    }
    const auto gesture = worker.LatestGesture();
    assert(gesture.tracked && gesture.hasAbsolutePose);
    assert(gesture.rotationXNormalized == 0.25f && gesture.rotationYNormalized == 0.75f && gesture.scale == 1.5f);
    elapsedMilliseconds.store(501);
    assert(!worker.LatestGesture().tracked);
    elapsedMilliseconds.store(0);

    worker.Submit({.timestampNanoseconds = 3U}, 2);
    {
        std::unique_lock lock{mutex};
        assert(processed.wait_for(lock, std::chrono::seconds{1}, [&] { return processedCount == 2; }));
    }
    assert(worker.LatestGesture().tracked);
    worker.Submit({.timestampNanoseconds = 4U}, 2);
    {
        std::unique_lock lock{mutex};
        assert(processed.wait_for(lock, std::chrono::seconds{1}, [&] { return processedCount == 3; }));
    }
    assert(!worker.LatestGesture().tracked);
    worker.Stop();
    return 0;
}
