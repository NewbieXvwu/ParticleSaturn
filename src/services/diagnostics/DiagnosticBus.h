#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace ParticleSaturn::Services::Diagnostics {

enum class Severity { Info, Warning, Error };

struct Record {
    std::string domain;
    std::string code;
    std::string message;
    Severity severity = Severity::Info;
    std::chrono::system_clock::time_point timestamp;
};

class DiagnosticBus {
public:
    static DiagnosticBus& Instance() { static DiagnosticBus instance; return instance; }
    void Publish(std::string domain, std::string code, std::string message, Severity severity) {
        std::lock_guard lock{mutex_};
        records_.push_back({std::move(domain), std::move(code), std::move(message), severity,
                            std::chrono::system_clock::now()});
        if (records_.size() > 256U) records_.erase(records_.begin());
    }
    std::vector<Record> Snapshot() const { std::lock_guard lock{mutex_}; return records_; }
private:
    mutable std::mutex mutex_;
    std::vector<Record> records_;
};

} // namespace ParticleSaturn::Services::Diagnostics
