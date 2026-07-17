#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <mach-o/dyld.h>
#include <string>
#include <sys/ucontext.h>
#include <unistd.h>
#include "MacOSCrashHandler.h"
#include "services/diagnostics/DiagnosticBus.h"

namespace ParticleSaturn::Services::Diagnostics::MacOS {
namespace {
int crashLogFd = -1;
std::uintptr_t mainImageLoadAddress = 0;

std::string CrashLogPath() {
    if (const char* overridePath = std::getenv("PARTICLESATURN_CRASH_LOG_PATH");
        overridePath != nullptr && overridePath[0] != '\0') return overridePath;
    const auto path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"ParticleSaturn-crash.log"];
    return [path fileSystemRepresentation];
}

void WriteHex(const char* prefix, std::size_t prefixLength, std::uintptr_t value) {
    if (crashLogFd < 0) return;
    char digits[sizeof(value) * 2U + 1U];
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < sizeof(value) * 2U; ++index) {
        const auto shift = static_cast<unsigned>((sizeof(value) * 2U - 1U - index) * 4U);
        digits[index] = hex[(value >> shift) & 0x0fU];
    }
    digits[sizeof(value) * 2U] = '\n';
    (void)write(crashLogFd, prefix, prefixLength);
    (void)write(crashLogFd, digits, sizeof(digits));
}

std::uintptr_t ProgramCounter(void* context) {
    if (context == nullptr) return 0;
    const auto* machineContext = static_cast<ucontext_t*>(context)->uc_mcontext;
#if defined(__aarch64__)
    return static_cast<std::uintptr_t>(machineContext->__ss.__pc);
#elif defined(__x86_64__)
    return static_cast<std::uintptr_t>(machineContext->__ss.__rip);
#else
    return 0;
#endif
}

void HandleSignal(int signalNumber, siginfo_t*, void* context) {
    if (crashLogFd >= 0) {
        const char* message = "ParticleSaturn fatal signal\n";
        std::size_t messageLength = sizeof("ParticleSaturn fatal signal\n") - 1U;
        switch (signalNumber) {
        case SIGSEGV:
            message = "ParticleSaturn fatal SIGSEGV\n";
            messageLength = sizeof("ParticleSaturn fatal SIGSEGV\n") - 1U;
            break;
        case SIGBUS:
            message = "ParticleSaturn fatal SIGBUS\n";
            messageLength = sizeof("ParticleSaturn fatal SIGBUS\n") - 1U;
            break;
        case SIGABRT:
            message = "ParticleSaturn fatal SIGABRT\n";
            messageLength = sizeof("ParticleSaturn fatal SIGABRT\n") - 1U;
            break;
        default: break;
        }
        (void)write(crashLogFd, message, messageLength);
        WriteHex("pc=0x", sizeof("pc=0x") - 1U, ProgramCounter(context));
        WriteHex("load=0x", sizeof("load=0x") - 1U, mainImageLoadAddress);
    }
    _exit(128 + signalNumber);
}

std::string ValueAfter(const std::string& text, const std::string& prefix) {
    const auto begin = text.rfind(prefix);
    if (begin == std::string::npos) return {};
    const auto valueBegin = begin + prefix.size();
    const auto end = text.find('\n', valueBegin);
    return text.substr(valueBegin, end == std::string::npos ? std::string::npos : end - valueBegin);
}

std::string Symbolize(const std::string& programCounter, const std::string& loadAddress) {
    if (programCounter.empty() || loadAddress.empty()) return {};
    NSString* executablePath = [[NSBundle mainBundle] executablePath];
    if (executablePath == nil) {
        const auto* arguments = [[NSProcessInfo processInfo] arguments];
        if ([arguments count] != 0) executablePath = [arguments objectAtIndex:0];
    }
    if (executablePath == nil) return {};
    auto* task = [[NSTask alloc] init];
    auto* output = [[NSPipe alloc] init];
    [task setLaunchPath:@"/usr/bin/atos"];
    [task setArguments:@[@"-o", executablePath, @"-l",
                         [NSString stringWithUTF8String:loadAddress.c_str()],
                         [NSString stringWithUTF8String:programCounter.c_str()]]];
    [task setStandardOutput:output];
    [task setStandardError:output];
    @try {
        [task launch];
        [task waitUntilExit];
    } @catch (NSException*) {
        [output release];
        [task release];
        return {};
    }
    NSData* data = [[output fileHandleForReading] readDataToEndOfFile];
    std::string result;
    if ([task terminationStatus] == 0 && [data length] != 0) {
        result.assign(static_cast<const char*>([data bytes]), [data length]);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    }
    [output release];
    [task release];
    return result;
}

void PublishPendingCrashLog(const std::string& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) return;
    const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    if (content.empty()) return;
    std::string message = content;
    const auto symbol = Symbolize(ValueAfter(content, "pc=0x"), ValueAfter(content, "load=0x"));
    if (!symbol.empty()) message += "symbol=" + symbol + '\n';
    DiagnosticBus::Instance().Publish("crash", "previous-fatal-signal", std::move(message), Severity::Error);
    std::ofstream clear{path, std::ios::binary | std::ios::trunc};
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
        const auto path = CrashLogPath();
        PublishPendingCrashLog(path);
        crashLogFd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    }
    mainImageLoadAddress = reinterpret_cast<std::uintptr_t>(_dyld_get_image_header(0));
    struct sigaction action {};
    action.sa_sigaction = &HandleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND | SA_SIGINFO;
    (void)sigaction(SIGSEGV, &action, nullptr);
    (void)sigaction(SIGBUS, &action, nullptr);
    (void)sigaction(SIGABRT, &action, nullptr);
}
} // namespace ParticleSaturn::Services::Diagnostics::MacOS
