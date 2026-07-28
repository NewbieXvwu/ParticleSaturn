#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace ParticleSaturn::Services::Diagnostics {

enum class Severity {
    Info,
    Warning,
    Error
};

struct Record {
    std::string                           domain;
    std::string                           code;
    std::string                           message;
    Severity                              severity = Severity::Info;
    std::chrono::system_clock::time_point timestamp;
};

class DiagnosticBus {
  public:
    static DiagnosticBus& Instance() {
        static DiagnosticBus instance;
        return instance;
    }

    void Publish(std::string domain, std::string code, std::string message, Severity severity) {
        std::lock_guard lock{mutex_};
        records_.push_back(
            {std::move(domain), std::move(code), std::move(message), severity, std::chrono::system_clock::now()});
        if (records_.size() > 256U) {
            records_.pop_front();
        }
    }

    // 全量快照仅供测试断言使用；每帧路径请用 Latest()/SnapshotSince()。
    std::vector<Record> Snapshot() const {
        std::lock_guard lock{mutex_};
        return {records_.begin(), records_.end()};
    }

    // 只取最新一条，供 UI 每帧显示——避免整表深拷贝（AUDIT P2-8）。
    bool Latest(Record& record) const {
        std::lock_guard lock{mutex_};
        if (records_.empty()) {
            return false;
        }
        record = records_.back();
        return true;
    }

    // 增量读取：返回时间戳晚于 since 的记录并推进 since；稳态下零拷贝。
    std::vector<Record> SnapshotSince(std::chrono::system_clock::time_point& since) const {
        std::lock_guard     lock{mutex_};
        std::vector<Record> result;
        for (const auto& record : records_) {
            if (record.timestamp <= since) {
                continue;
            }
            result.push_back(record);
        }
        if (!result.empty()) {
            since = result.back().timestamp;
        }
        return result;
    }

  private:
    mutable std::mutex mutex_;
    std::deque<Record> records_;
};

} // namespace ParticleSaturn::Services::Diagnostics
