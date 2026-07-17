#import <Cocoa/Cocoa.h>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include "MacOSCrashHandler.h"
#include "services/diagnostics/DiagnosticBus.h"

namespace ParticleSaturn::Services::Diagnostics::MacOS {
namespace {
int crashLogFd = -1;

void HandleSignal(int signalNumber) {
    if (crashLogFd >= 0) {
        const char* message = "ParticleSaturn fatal signal\n";
        switch (signalNumber) {
        case SIGSEGV: message = "ParticleSaturn fatal SIGSEGV\n"; break;
        case SIGBUS: message = "ParticleSaturn fatal SIGBUS\n"; break;
        case SIGABRT: message = "ParticleSaturn fatal SIGABRT\n"; break;
        default: break;
        }
        (void)write(crashLogFd, message, __builtin_strlen(message));
    }
    _exit(128 + signalNumber);
}

void HandleException(NSException* exception) {
    const char* reason = [[exception reason] UTF8String];
    DiagnosticBus::Instance().Publish("crash", [[exception name] UTF8String], reason == nullptr ? "uncaught Objective-C exception" : reason,
                                      Severity::Error);
}
} // namespace
void InstallCrashHandler() {
    NSSetUncaughtExceptionHandler(&HandleException);
    if (crashLogFd < 0) {
        const auto path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ParticleSaturn-crash.log"];
        crashLogFd = open([path fileSystemRepresentation], O_WRONLY | O_CREAT | O_APPEND, 0600);
    }
    struct sigaction action {};
    action.sa_handler = &HandleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;
    (void)sigaction(SIGSEGV, &action, nullptr);
    (void)sigaction(SIGBUS, &action, nullptr);
    (void)sigaction(SIGABRT, &action, nullptr);
}
} // namespace ParticleSaturn::Services::Diagnostics::MacOS
