#include "services/diagnostics/DiagnosticBus.h"
#include "services/diagnostics/macos/MacOSCrashHandler.h"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

__attribute__((noinline)) void CrashNow() {
    volatile std::uintptr_t address = 0;
    *reinterpret_cast<volatile std::uint32_t*>(address) = 1U;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string{argv[1]} == "--crash-child") {
        setenv("PARTICLESATURN_CRASH_LOG_PATH", argv[2], 1);
        ParticleSaturn::Services::Diagnostics::MacOS::InstallCrashHandler();
        CrashNow();
        return 1;
    }

    assert(argc == 1);
    const auto path = std::filesystem::temp_directory_path() /
        ("ParticleSaturnCrashHandlerTests-" + std::to_string(static_cast<long long>(getpid())) + ".log");
    std::filesystem::remove(path);
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(argv[0], argv[0], "--crash-child", path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 128 + SIGSEGV);

    std::ifstream rawInput{path, std::ios::binary};
    const std::string raw{std::istreambuf_iterator<char>{rawInput}, std::istreambuf_iterator<char>{}};
    assert(raw.find("ParticleSaturn fatal SIGSEGV") != std::string::npos);
    assert(raw.find("pc=0x") != std::string::npos);
    assert(raw.find("load=0x") != std::string::npos);

    setenv("PARTICLESATURN_CRASH_LOG_PATH", path.c_str(), 1);
    ParticleSaturn::Services::Diagnostics::MacOS::InstallCrashHandler();
    const auto records = ParticleSaturn::Services::Diagnostics::DiagnosticBus::Instance().Snapshot();
    assert(!records.empty());
    const auto& crash = records.back();
    assert(crash.domain == "crash");
    assert(crash.code == "previous-fatal-signal");
    assert(crash.severity == ParticleSaturn::Services::Diagnostics::Severity::Error);
    assert(crash.message.find("symbol=") != std::string::npos);
    assert(crash.message.find("CrashNow") != std::string::npos);

    std::ifstream clearedInput{path, std::ios::binary};
    const std::string cleared{std::istreambuf_iterator<char>{clearedInput}, std::istreambuf_iterator<char>{}};
    assert(cleared.empty());
    std::filesystem::remove(path);
    return 0;
}
