#include "HandTrackerController.h"

#include <windows.h>

#include <chrono>

#include "../AppState.h"
#include "../ErrorHandler.h"
#include "../Localization.h"
#include "CameraSelector/CameraEnumerator.h"
#include "CameraSelector/CameraSelector.h"
#include "HandTracker.h"

namespace ParticleSaturn::HandTracking {

namespace {

#if !defined(HANDTRACKER_STATIC)
// Function pointer types for runtime DLL loading.
using FnInitTracker                     = bool (*)(int camera_id, const char* model_dir);
using FnIsTrackerReady                  = int (*)();
using FnGetTrackerLastError             = int (*)();
using FnGetTrackerLastErrorMessage      = const char* (*)();
using FnGetHandData                     = bool (*)(float* out_scale, float* out_rot_x, float* out_rot_y, bool* out_has_hand);
using FnReleaseTracker                  = void (*)();
using FnSetTrackerDebugMode             = void (*)(bool enabled);
using FnGetTrackerDebugMode             = bool (*)();
using FnSetTrackerSIMDMode              = void (*)(int mode);
using FnGetTrackerSIMDMode              = int (*)();
using FnGetTrackerSIMDImplementation    = const char* (*)();
using FnSetHandLostDelayFrames          = void (*)(int frames);
using FnGetHandLostDelayFrames          = int (*)();

struct Api {
    HMODULE                      module                   = nullptr;
    FnInitTracker                InitTracker              = nullptr;
    FnIsTrackerReady             IsTrackerReady           = nullptr;
    FnGetTrackerLastError        GetTrackerLastError      = nullptr;
    FnGetTrackerLastErrorMessage GetTrackerLastErrorMessage = nullptr;
    FnGetHandData                GetHandData              = nullptr;
    FnReleaseTracker             ReleaseTracker           = nullptr;
    FnSetTrackerDebugMode        SetTrackerDebugMode      = nullptr;
    FnGetTrackerDebugMode        GetTrackerDebugMode      = nullptr;
    FnSetTrackerSIMDMode         SetTrackerSIMDMode       = nullptr;
    FnGetTrackerSIMDMode         GetTrackerSIMDMode       = nullptr;
    FnGetTrackerSIMDImplementation GetTrackerSIMDImplementation = nullptr;
    FnSetHandLostDelayFrames     SetTrackerHandLostDelayFrames = nullptr;
    FnGetHandLostDelayFrames     GetTrackerHandLostDelayFrames = nullptr;
};

// Keep the module loaded for the process lifetime:
// - HandTracker::ReleaseTracker may detach the worker thread on timeout.
// - Unloading a module while its code may still be executing is unsafe.
static Api g_api{};

template <typename T>
T LoadProc(HMODULE mod, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(mod, name));
}
#endif

} // namespace

Controller::~Controller() {
    Shutdown();
}

bool Controller::Init(HWND hwnd, AppState* state) {
    hwnd_     = hwnd;
    appState_ = state;
    status_.store(Status::NotStarted);
    latestSample_ = Sample{};
    return EnsureApiLoaded();
}

void Controller::Shutdown() {
    StopReaderThread();

    // Best-effort cleanup: stop the tracker as well (static and dll mode).
#if defined(HANDTRACKER_STATIC)
    if (apiLoaded_) {
        ReleaseTracker();
    }
#else
    if (apiLoaded_ && g_api.ReleaseTracker != nullptr) {
        g_api.ReleaseTracker();
    }
    // Do NOT FreeLibrary; see comment above.
#endif

    hwnd_     = nullptr;
    appState_ = nullptr;
    status_.store(Status::NotStarted);
    apiLoaded_ = false;
}

bool Controller::EnsureApiLoaded() {
    if (apiLoaded_) {
        return true;
    }

#if defined(HANDTRACKER_STATIC)
    apiLoaded_ = true;
    return true;
#else
    if (g_api.module == nullptr) {
        const std::wstring dllPath = GetExeDirW() + L"\\HandTracker.dll";
        g_api.module               = LoadLibraryW(dllPath.c_str());
        if (g_api.module == nullptr) {
            status_.store(Status::Unavailable);
            apiLoaded_ = false;
            return false;
        }
    }

    // Required functions (minimal set for core tracking).
    g_api.InitTracker              = LoadProc<FnInitTracker>(g_api.module, "InitTracker");
    g_api.IsTrackerReady           = LoadProc<FnIsTrackerReady>(g_api.module, "IsTrackerReady");
    g_api.GetTrackerLastError      = LoadProc<FnGetTrackerLastError>(g_api.module, "GetTrackerLastError");
    g_api.GetTrackerLastErrorMessage =
        LoadProc<FnGetTrackerLastErrorMessage>(g_api.module, "GetTrackerLastErrorMessage");
    g_api.GetHandData              = LoadProc<FnGetHandData>(g_api.module, "GetHandData");
    g_api.ReleaseTracker           = LoadProc<FnReleaseTracker>(g_api.module, "ReleaseTracker");

    // Optional functions (UI/debug).
    g_api.SetTrackerDebugMode      = LoadProc<FnSetTrackerDebugMode>(g_api.module, "SetTrackerDebugMode");
    g_api.GetTrackerDebugMode      = LoadProc<FnGetTrackerDebugMode>(g_api.module, "GetTrackerDebugMode");
    g_api.SetTrackerSIMDMode       = LoadProc<FnSetTrackerSIMDMode>(g_api.module, "SetTrackerSIMDMode");
    g_api.GetTrackerSIMDMode       = LoadProc<FnGetTrackerSIMDMode>(g_api.module, "GetTrackerSIMDMode");
    g_api.GetTrackerSIMDImplementation =
        LoadProc<FnGetTrackerSIMDImplementation>(g_api.module, "GetTrackerSIMDImplementation");
    g_api.SetTrackerHandLostDelayFrames =
        LoadProc<FnSetHandLostDelayFrames>(g_api.module, "SetTrackerHandLostDelayFrames");
    g_api.GetTrackerHandLostDelayFrames =
        LoadProc<FnGetHandLostDelayFrames>(g_api.module, "GetTrackerHandLostDelayFrames");

    const bool ok = g_api.InitTracker && g_api.IsTrackerReady && g_api.GetHandData && g_api.ReleaseTracker &&
                    g_api.GetTrackerLastError && g_api.GetTrackerLastErrorMessage;
    if (!ok) {
        status_.store(Status::Unavailable);
        apiLoaded_ = false;
        return false;
    }

    apiLoaded_ = true;
    return true;
#endif
}

std::wstring Controller::GetExeDirW() const {
    wchar_t buf[MAX_PATH] = {};
    DWORD   n             = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return L".";
    }
    std::wstring path(buf, buf + n);
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, pos);
}

std::string Controller::WideToUtf8(const std::wstring& w) const {
    if (w.empty()) {
        return {};
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool Controller::StartWithCameraSelector(bool forceShowDialog) {
    if (!EnsureApiLoaded()) {
        return false;
    }

    StopReaderThread();
    status_.store(Status::Starting);

    // 启动行为：如果有多个摄像头则强制弹出选择器（避免“默认 #0”导致用户误以为没生效）。
    // 注：forceShowDialog=true 也会绕过“记住我的选择/不再询问”，用于显式重启或强制选择。
    bool shouldForceShow = forceShowDialog;
    if (!shouldForceShow) {
        const int camCount = CameraSelector::GetCameraCount();
        shouldForceShow    = (camCount > 1);
    }

    const int cam = CameraSelector::ShowCameraSelectorDialog(hwnd_, GetModuleHandleW(nullptr), shouldForceShow);
    selectedCamera_.store(cam < 0 ? 0 : cam);

    ApplyHandParamsFromAppState();

    const std::string modelDirUtf8 = WideToUtf8(GetExeDirW());
    ErrorHandler::SetStage(ErrorHandler::AppStage::HAND_TRACKER_INIT);
#if defined(HANDTRACKER_STATIC)
    if (!InitTracker(selectedCamera_.load(), modelDirUtf8.c_str())) {
#else
    if (!g_api.InitTracker(selectedCamera_.load(), modelDirUtf8.c_str())) {
#endif
        status_.store(Status::Failed);
        return false;
    }

    // init is async; we transition to Ready in Tick().
    return true;
}

bool Controller::RestartWithCameraSelector(bool forceShowDialog) {
    if (!EnsureApiLoaded()) {
        return false;
    }

    StopReaderThread();
#if defined(HANDTRACKER_STATIC)
    ReleaseTracker();
#else
    if (g_api.ReleaseTracker) {
        g_api.ReleaseTracker();
    }
#endif
    status_.store(Status::NotStarted);

    return StartWithCameraSelector(forceShowDialog);
}

void Controller::Tick() {
    if (!apiLoaded_) {
        return;
    }

    ApplyHandParamsFromAppState();

    const Status st = status_.load();
    if (st != Status::Starting) {
        return;
    }

#if defined(HANDTRACKER_STATIC)
    const int ready = IsTrackerReady();
#else
    const int ready = g_api.IsTrackerReady ? g_api.IsTrackerReady() : -1;
#endif
    if (ready == 0) {
        return;
    }
    if (ready == 1) {
        status_.store(Status::Ready);
        StartReaderThreadIfNeeded();
        ErrorHandler::SetCameraInfo(selectedCamera_.load(), 640, 480, true);
    } else {
        status_.store(Status::Failed);
    }
}

void Controller::StartReaderThreadIfNeeded() {
    if (readerRunning_.load()) {
        return;
    }
    readerRunning_.store(true);
    readerThread_ = std::thread(&Controller::ReaderLoop, this);
}

void Controller::StopReaderThread() {
    readerRunning_.store(false);
    if (readerThread_.joinable()) {
        readerThread_.join();
    }
}

void Controller::ReaderLoop() {
    using namespace std::chrono_literals;
    while (readerRunning_.load()) {
        Sample s{};
        bool   has = false;
#if defined(HANDTRACKER_STATIC)
        GetHandData(&s.scale, &s.rotX, &s.rotY, &has);
#else
        if (g_api.GetHandData) {
            g_api.GetHandData(&s.scale, &s.rotX, &s.rotY, &has);
        }
#endif
        s.hasHand = has;
        {
            std::lock_guard<std::mutex> lock(sampleMutex_);
            latestSample_ = s;
        }
        std::this_thread::sleep_for(16ms);
    }
}

Sample Controller::GetLatestSample() const {
    std::lock_guard<std::mutex> lock(sampleMutex_);
    return latestSample_;
}

bool Controller::GetDebugMode(bool* outEnabled) const {
    if (!apiLoaded_ || outEnabled == nullptr) {
        return false;
    }
#if defined(HANDTRACKER_STATIC)
    *outEnabled = GetTrackerDebugMode();
    return true;
#else
    if (g_api.GetTrackerDebugMode == nullptr) {
        return false;
    }
    *outEnabled = g_api.GetTrackerDebugMode();
    return true;
#endif
}

void Controller::SetDebugMode(bool enabled) {
    if (!apiLoaded_) {
        return;
    }
#if defined(HANDTRACKER_STATIC)
    SetTrackerDebugMode(enabled);
    return;
#else
    if (g_api.SetTrackerDebugMode == nullptr) {
        return;
    }
    g_api.SetTrackerDebugMode(enabled);
#endif
}

bool Controller::GetSIMDMode(int* outMode) const {
    if (!apiLoaded_ || outMode == nullptr) {
        return false;
    }
#if defined(HANDTRACKER_STATIC)
    *outMode = GetTrackerSIMDMode();
    return true;
#else
    if (g_api.GetTrackerSIMDMode == nullptr) {
        return false;
    }
    *outMode = g_api.GetTrackerSIMDMode();
    return true;
#endif
}

void Controller::SetSIMDMode(int mode) {
    if (!apiLoaded_) {
        return;
    }
#if defined(HANDTRACKER_STATIC)
    SetTrackerSIMDMode(mode);
    return;
#else
    if (g_api.SetTrackerSIMDMode == nullptr) {
        return;
    }
    g_api.SetTrackerSIMDMode(mode);
#endif
}

std::string Controller::GetSIMDImplementation() const {
    if (!apiLoaded_) {
        return {};
    }
#if defined(HANDTRACKER_STATIC)
    const char* s = GetTrackerSIMDImplementation();
    return s ? std::string(s) : std::string();
#else
    if (g_api.GetTrackerSIMDImplementation == nullptr) {
        return {};
    }
    const char* s = g_api.GetTrackerSIMDImplementation();
    return s ? std::string(s) : std::string();
#endif
}

int Controller::GetLastErrorCode() const {
    if (!apiLoaded_) {
        return HANDTRACKER_ERROR_UNKNOWN;
    }
#if defined(HANDTRACKER_STATIC)
    return GetTrackerLastError();
#else
    if (g_api.GetTrackerLastError == nullptr) {
        return HANDTRACKER_ERROR_UNKNOWN;
    }
    return g_api.GetTrackerLastError();
#endif
}

std::string Controller::GetLastErrorMessageUtf8() const {
    if (!apiLoaded_) {
        return {};
    }
#if defined(HANDTRACKER_STATIC)
    const char* s = GetTrackerLastErrorMessage();
    return s ? std::string(s) : std::string();
#else
    if (g_api.GetTrackerLastErrorMessage == nullptr) {
        return {};
    }
    const char* s = g_api.GetTrackerLastErrorMessage();
    return s ? std::string(s) : std::string();
#endif
}

void Controller::ApplyHandParamsFromAppState() {
    if (!apiLoaded_ || appState_ == nullptr) {
        return;
    }
    const int desired = appState_->handParams.handLostDelay;
    if (desired != lastHandLostDelayFrames_) {
#if defined(HANDTRACKER_STATIC)
        SetTrackerHandLostDelayFrames(desired);
#else
        if (g_api.SetTrackerHandLostDelayFrames != nullptr) {
            g_api.SetTrackerHandLostDelayFrames(desired);
        } else {
            return;
        }
#endif
        lastHandLostDelayFrames_ = desired;
    }
}

} // namespace ParticleSaturn::HandTracking
