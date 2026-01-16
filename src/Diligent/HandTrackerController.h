#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct AppState;

struct HWND__;
using HWND = HWND__*;

namespace ParticleSaturn::HandTracking {

struct Sample {
    bool  hasHand = false;
    float scale   = 1.0f;
    float rotX    = 0.5f;
    float rotY    = 0.5f;
};

enum class Status {
    Unavailable,
    NotStarted,
    Starting,
    Ready,
    Failed,
};

class Controller final {
  public:
    Controller() = default;
    ~Controller();

    Controller(const Controller&)            = delete;
    Controller& operator=(const Controller&) = delete;
    Controller(Controller&&)                 = delete;
    Controller& operator=(Controller&&)      = delete;

    bool Init(HWND hwnd, AppState* state);
    void Shutdown();

    // Start tracking (shows camera selector if needed). Non-blocking init afterwards.
    bool StartWithCameraSelector(bool forceShowDialog);

    // Restart tracking: Stop reader thread -> ReleaseTracker -> choose camera -> InitTracker.
    bool RestartWithCameraSelector(bool forceShowDialog);

    // Per-frame polling: transitions Starting -> Ready/Failed.
    void Tick();

    Status GetStatus() const { return status_.load(); }

    int GetSelectedCamera() const { return selectedCamera_.load(); }

    Sample GetLatestSample() const;

    // Debug/advanced controls (no-op if unavailable).
    bool GetDebugMode(bool* outEnabled) const;
    void SetDebugMode(bool enabled);

    bool GetSIMDMode(int* outMode) const;
    void SetSIMDMode(int mode);
    std::string GetSIMDImplementation() const;

    int GetLastErrorCode() const;
    std::string GetLastErrorMessageUtf8() const;

    void ApplyHandParamsFromAppState(); // handLostDelay currently lives in HandTracker library

  private:
    bool EnsureApiLoaded();
    void StopReaderThread();
    void StartReaderThreadIfNeeded();

    std::wstring GetExeDirW() const;
    std::string  WideToUtf8(const std::wstring& w) const;

    // Thread loop reading GetHandData() at ~60Hz.
    void ReaderLoop();

    std::atomic<Status> status_{Status::NotStarted};
    std::atomic<int>    selectedCamera_{0};
    std::atomic<bool>   readerRunning_{false};

    mutable std::mutex sampleMutex_;
    Sample             latestSample_;
    std::thread        readerThread_;

    HWND     hwnd_     = nullptr;
    AppState* appState_ = nullptr;

    // Cache hand params to avoid redundant API calls every frame.
    int lastHandLostDelayFrames_ = -1;

    // Runtime-loaded API (dll mode). In static mode, pointers are unused.
    bool apiLoaded_ = false;
};

} // namespace ParticleSaturn::HandTracking

