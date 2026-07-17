#include "HandTrackingWorker.h"

#include <algorithm>
#include <iostream>
#include <utility>

namespace ParticleSaturn::Services::HandTracking::MacOS {

HandTrackingWorker::HandTrackingWorker(Processor processor, Now now)
    : processor_{std::move(processor)}, now_{std::move(now)} {
    if (!now_) now_ = [] { return std::chrono::steady_clock::now(); };
}

HandTrackingWorker::~HandTrackingWorker() { Stop(); }

void HandTrackingWorker::Start() {
    if (worker_.joinable()) return;
    stopping_ = false;
    worker_ = std::thread([this] { Run(); });
}

void HandTrackingWorker::Stop() {
    {
        std::lock_guard lock{inputMutex_};
        stopping_ = true;
        inputReady_.notify_one();
    }
    if (worker_.joinable()) worker_.join();
}

void HandTrackingWorker::Submit(Camera::Frame frame, int handLostDelay) {
    std::lock_guard lock{inputMutex_};
    pendingFrame_ = std::move(frame);
    pendingLostDelay_ = std::clamp(handLostDelay, 1, 120);
    hasPendingFrame_ = true;
    inputReady_.notify_one();
}

App::GestureInput HandTrackingWorker::LatestGesture() const {
    std::lock_guard lock{outputMutex_};
    App::GestureInput gesture;
    if (!sample_.tracked || now_() - sample_.updatedAt > std::chrono::milliseconds{500}) return gesture;
    gesture.tracked = true;
    gesture.hasAbsolutePose = true;
    gesture.rotationXNormalized = sample_.pose.centerX;
    gesture.rotationYNormalized = sample_.pose.centerY;
    gesture.scale = sample_.pose.scale;
    return gesture;
}

void HandTrackingWorker::Run() {
    int lostFrames = 0;
    for (;;) {
        Camera::Frame frame;
        int handLostDelay = 1;
        {
            std::unique_lock lock{inputMutex_};
            inputReady_.wait(lock, [this] { return stopping_ || hasPendingFrame_; });
            if (stopping_) return;
            frame = std::move(pendingFrame_);
            handLostDelay = pendingLostDelay_;
            hasPendingFrame_ = false;
        }
        std::string error;
        HandPose pose;
        const bool tracked = processor_ && processor_(frame, pose, error);
        if (!tracked && !error.empty() && error != lastRuntimeError_) {
            std::clog << "[HandTracking] " << error << '\n';
            lastRuntimeError_ = error;
        }
        if (tracked) lastRuntimeError_.clear();
        std::lock_guard lock{outputMutex_};
        sample_.updatedAt = now_();
        if (tracked) {
            lostFrames = 0;
            sample_.tracked = true;
            sample_.pose = pose;
        } else if (++lostFrames >= handLostDelay) {
            sample_.tracked = false;
        }
    }
}

} // namespace ParticleSaturn::Services::HandTracking::MacOS
