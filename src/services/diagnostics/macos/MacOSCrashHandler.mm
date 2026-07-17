#import <Cocoa/Cocoa.h>

#include "MacOSCrashHandler.h"
#include "services/diagnostics/DiagnosticBus.h"

namespace ParticleSaturn::Services::Diagnostics::MacOS {
namespace {
void HandleException(NSException* exception) {
    const char* reason = [[exception reason] UTF8String];
    DiagnosticBus::Instance().Publish("crash", [[exception name] UTF8String], reason == nullptr ? "uncaught Objective-C exception" : reason,
                                      Severity::Error);
}
} // namespace
void InstallCrashHandler() { NSSetUncaughtExceptionHandler(&HandleException); }
} // namespace ParticleSaturn::Services::Diagnostics::MacOS
