#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "app/FrameCoordinator.h"
#include "services/camera/CameraCapture.h"
#include "services/hand_tracking/macos/XnnpackRuntime.h"

namespace ParticleSaturn::Services::HandTracking::MacOS {

class HandTrackingWorker {
  public:
    using Processor = std::function<bool(const Camera::Frame&, HandPose&, std::string&)>;
    using Now       = std::function<std::chrono::steady_clock::time_point()>;

    explicit HandTrackingWorker(Processor processor, Now now = {});
    ~HandTrackingWorker();

    HandTrackingWorker(const HandTrackingWorker&)            = delete;
    HandTrackingWorker& operator=(const HandTrackingWorker&) = delete;

    void              Start();
    void              Stop();
    void              Submit(Camera::Frame frame, int handLostDelay);
    App::GestureInput LatestGesture() const;

  private:
    struct Sample {
        bool                                  tracked = false;
        HandPose                              pose;
        std::chrono::steady_clock::time_point updatedAt{};
    };

    void Run();

    Processor               processor_;
    Now                     now_;
    mutable std::mutex      inputMutex_;
    mutable std::mutex      outputMutex_;
    std::condition_variable inputReady_;
    std::thread             worker_;
    Camera::Frame           pendingFrame_;
    Sample                  sample_;
    std::string             lastRuntimeError_;
    int                     pendingLostDelay_ = 1;
    bool                    hasPendingFrame_  = false;
    bool                    stopping_         = false;
};

} // namespace ParticleSaturn::Services::HandTracking::MacOS
