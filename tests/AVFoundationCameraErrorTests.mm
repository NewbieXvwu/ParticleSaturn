#import <Foundation/Foundation.h>

#include "services/camera/macos/AVFoundationCamera.h"
#include "services/diagnostics/DiagnosticBus.h"

#include <cassert>
#include <cstddef>

int main() {
    @autoreleasepool {
        namespace Cam = ParticleSaturn::Services::Camera;
        namespace Diag = ParticleSaturn::Services::Diagnostics;

        const auto baseline = Diag::DiagnosticBus::Instance().Snapshot().size();

        Cam::MacOS::AVFoundationCamera camera;

        // A freshly constructed camera has no frame and is not running.
        Cam::Frame frame;
        assert(!camera.LatestFrame(frame));
        assert(!camera.IsRunning());

        // Starting a nonexistent device must fail and record a camera error.
        // On machines that have not authorised the camera the failure is
        // classified as "permission"; on authorised machines the bogus unique
        // id resolves to "device-unavailable".  Either path exercises the
        // structured error-publishing contract.
        const bool started = camera.Start("particlesaturn-nonexistent-device", 1280, 720);
        assert(!started);
        assert(!camera.IsRunning());
        assert(!camera.LastError().empty());

        const auto afterStart = Diag::DiagnosticBus::Instance().Snapshot();
        assert(afterStart.size() > baseline);
        bool foundCameraError = false;
        for (std::size_t i = baseline; i < afterStart.size(); ++i) {
            const auto& record = afterStart[i];
            if (record.domain != "camera") continue;
            assert(record.severity == Diag::Severity::Error);
            assert(record.code == "permission" || record.code == "device-unavailable");
            assert(!record.message.empty());
            foundCameraError = true;
        }
        assert(foundCameraError);

        // The disconnect handler is a safe no-op when no session is active and
        // must not emit a spurious diagnostic.
        const auto beforeDisconnect = Diag::DiagnosticBus::Instance().Snapshot().size();
        camera.HandleDeviceDisconnected("particlesaturn-nonexistent-device");
        assert(Diag::DiagnosticBus::Instance().Snapshot().size() == beforeDisconnect);

        // Stop is safe and idempotent after a failed start.
        camera.Stop();
        camera.Stop();
        assert(!camera.IsRunning());
    }
    return 0;
}
