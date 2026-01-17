#pragma once
// 调试日志系统 - 带 ImGui 显示的日志记录
//
// 目标：
// - Diligent 版可在 ImGui 中看到日志（通过重定向 stdout/stderr）
// - 降噪（重复日志合并、一次性日志）
// - 支持 Info/Warn/Error 的过滤、搜索、复制

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <streambuf>
#include <string>
#include <unordered_set>

#include "md3/MD3.h"

// 日志级别
enum class LogLevel {
    Info = 0,
    Warn = 1,
    Error = 2,
};

// 日志条目
struct LogEntry {
    std::string message;
    LogLevel    level       = LogLevel::Info;
    uint32_t    repeatCount = 1;
    uint64_t    timeMs      = 0; // since DebugLog start
};

class DebugLog {
  public:
    static DebugLog& Instance() {
        static DebugLog inst;
        return inst;
    }

    // 兼容旧用法：自动检测级别（仅提升等级）
    void Add(const std::string& msg) { Add(LogLevel::Info, msg, true); }

    // 显式级别
    void Add(LogLevel level, const std::string& msg) { Add(level, msg, true); }

    // 仅记录一次（按 key 去重），用于避免每帧刷屏
    void AddOnce(const char* key, LogLevel level, const std::string& msg) {
        if (key == nullptr || key[0] == '\0') {
            Add(level, msg);
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        InitStartTimeIfNeeded();
        if (m_onceKeys.insert(key).second) {
            AddLocked(msg, level);
        }
    }

    // 便于 stdout/stderr 捕获：指定默认级别（仍会根据内容提升）
    void AddFromStream(LogLevel defaultLevel, const std::string& msg) {
        Add(defaultLevel, msg, true);
    }

    // 绘制日志（带过滤和搜索）
    void Draw(const char* searchFilter, int levelFilter) {
        // 复刻 OpenGL 版：日志框需要有内边距，并且滚动条不能突破圆角边界。
        // - 内边距：避免文字贴边
        // - 滚动条：关闭 ImGui 自带滚动条，改用 MD3::WindowScrollbar 自绘（与主窗口一致）
        float dpi = MD3::GetContext().dpiScale;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * dpi, 10.0f * dpi));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f * dpi);

        ImGui::BeginChild("LogScroll",
                          ImVec2(0, 200),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);

        std::lock_guard<std::mutex> lock(m_mutex);
        // 两列：左侧时间轴（固定宽度），右侧消息（自动换行）
        // 要求：
        // - 时间轴与正文之间不要留太大空隙
        // - 换行后第二行与第一行正文对齐（也就是对齐到时间轴后方）
        //
        // 时间列宽度按“当前最大时间戳”动态估算，并限制上下界，避免像 "[99999.999s]" 这种过宽预留导致间距过大。
        float timeColWidth = 0.0f;
        {
            const uint64_t maxMs = !m_entries.empty() ? m_entries.back().timeMs : 0;
            const double   sec   = static_cast<double>(maxMs) / 1000.0;
            char           buf[32]{};
            std::snprintf(buf, sizeof(buf), "[%.3fs]", sec);

            const float minW = ImGui::CalcTextSize("[0.000s]").x;
            const float maxW = ImGui::CalcTextSize("[9999.999s]").x;
            timeColWidth     = ImClamp(ImGui::CalcTextSize(buf).x, minW, maxW) + 4.0f * dpi;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * dpi, 2.0f * dpi));
        if (ImGui::BeginTable("##LogTable",
                              2,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("##Time", ImGuiTableColumnFlags_WidthFixed, timeColWidth);
            ImGui::TableSetupColumn("##Msg", ImGuiTableColumnFlags_WidthStretch);

            for (size_t i = 0; i < m_entries.size(); ++i) {
                const auto& entry = m_entries[i];

                // 级别过滤: 0=全部, 1=Info, 2=Warn, 3=Error
                if (levelFilter > 0) {
                    if (levelFilter == 1 && entry.level != LogLevel::Info) {
                        continue;
                    }
                    if (levelFilter == 2 && entry.level != LogLevel::Warn) {
                        continue;
                    }
                    if (levelFilter == 3 && entry.level != LogLevel::Error) {
                        continue;
                    }
                }

                // 搜索过滤（在 message 上做匹配，避免时间前缀影响搜索）
                if (searchFilter && searchFilter[0] != '\0') {
                    if (entry.message.find(searchFilter) == std::string::npos) {
                        continue;
                    }
                }

                // 根据级别设置颜色
                ImVec4 color;
                switch (entry.level) {
                case LogLevel::Warn:
                    color = ImVec4(1.0f, 0.8f, 0.0f, 1.0f); // 黄色
                    break;
                case LogLevel::Error:
                    color = ImVec4(1.0f, 0.3f, 0.3f, 1.0f); // 红色
                    break;
                default:
                    color = ImGui::GetStyleColorVec4(ImGuiCol_Text); // 默认颜色
                    break;
                }

                ImGui::TableNextRow();

                // 时间列（灰色）
                ImGui::TableNextColumn();
                if (m_startTimeInited) {
                    const double sec = static_cast<double>(entry.timeMs) / 1000.0;
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    ImGui::Text("[%.3fs]", sec);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextUnformatted("");
                }

                // 消息列（自动换行，换行后保持对齐到时间轴后方）
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, color);
                ImGui::PushTextWrapPos(0.0f);
                if (entry.repeatCount > 1) {
                    std::string msg = entry.message;
                    msg += "  x";
                    msg += std::to_string(entry.repeatCount);
                    ImGui::TextUnformatted(msg.c_str());
                } else {
                    ImGui::TextUnformatted(entry.message.c_str());
                }
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        if (m_scrollToBottom && !m_paused) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }

        // 在 Child 内自绘滚动条（titleBarHeight=0）
        MD3::WindowScrollbar(0.0f);

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
    }

    // 保持向后兼容的简单绘制
    void Draw() { Draw(nullptr, 0); }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_onceKeys.clear();
    }

    std::string GetAllText() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string                 result;
        for (const auto& entry : m_entries) {
            AppendLine(result, entry);
        }
        return result;
    }

    // 获取过滤后的文本（用于复制）
    std::string GetFilteredText(const char* searchFilter, int levelFilter) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string                 result;
        for (const auto& entry : m_entries) {
            if (levelFilter > 0) {
                if (levelFilter == 1 && entry.level != LogLevel::Info) {
                    continue;
                }
                if (levelFilter == 2 && entry.level != LogLevel::Warn) {
                    continue;
                }
                if (levelFilter == 3 && entry.level != LogLevel::Error) {
                    continue;
                }
            }
            if (searchFilter && searchFilter[0] != '\0') {
                if (entry.message.find(searchFilter) == std::string::npos) {
                    continue;
                }
            }
            AppendLine(result, entry);
        }
        return result;
    }

    void SetPaused(bool paused) { m_paused = paused; }

    bool IsPaused() const { return m_paused; }

  private:
    DebugLog() = default;

    static void StripAnsiEscapesInPlace(std::string& s) {
        // Remove ANSI escape sequences (e.g. "\x1b[39m") so ImGui log doesn't show "[39m" noise.
        // Handles:
        // - CSI: ESC [ ... <final byte>
        // - OSC: ESC ] ... BEL
        // Also removes stray "[39m"/"[0m" fragments (some copy paths may drop the ESC byte).
        size_t w = 0;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == 0x1B) { // ESC
                if (i + 1 < s.size() && s[i + 1] == '[') { // CSI
                    i += 2;
                    // Parameter bytes 0x30-0x3F, intermediate bytes 0x20-0x2F, final byte 0x40-0x7E
                    while (i < s.size()) {
                        const unsigned char b = static_cast<unsigned char>(s[i]);
                        if (b >= 0x40 && b <= 0x7E) {
                            i++;
                            break;
                        }
                        i++;
                    }
                    continue;
                }
                if (i + 1 < s.size() && s[i + 1] == ']') { // OSC
                    i += 2;
                    while (i < s.size() && static_cast<unsigned char>(s[i]) != 0x07) { // BEL
                        i++;
                    }
                    if (i < s.size()) {
                        i++; // consume BEL
                    }
                    continue;
                }
                // Unknown escape, drop ESC.
                i++;
                continue;
            }

            if (s[i] == '[') {
                // Remove stray "[<digits/;>m" (e.g. "[39m", "[0m", "[1;32m")
                size_t j        = i + 1;
                bool   hasDigit = false;
                while (j < s.size()) {
                    const char ch = s[j];
                    if (ch >= '0' && ch <= '9') {
                        hasDigit = true;
                        j++;
                        continue;
                    }
                    if (ch == ';') {
                        j++;
                        continue;
                    }
                    break;
                }
                if (hasDigit && j < s.size() && s[j] == 'm') {
                    i = j + 1;
                    continue;
                }
            }

            s[w++] = s[i++];
        }
        s.resize(w);
    }

    void Add(LogLevel level, const std::string& msg, bool autoDetect) {
        std::lock_guard<std::mutex> lock(m_mutex);
        InitStartTimeIfNeeded();
        LogLevel finalLevel = level;
        if (autoDetect) {
            const LogLevel detected = DetectLevel(msg);
            if (static_cast<int>(detected) > static_cast<int>(finalLevel)) {
                finalLevel = detected;
            }
        }
        AddLocked(msg, finalLevel);
    }

    void AddLocked(const std::string& msg, LogLevel level) {
        if (msg.empty()) {
            return;
        }

        std::string normalized = msg;
        StripAnsiEscapesInPlace(normalized);

        if (normalized.empty()) {
            return;
        }

        if (!m_entries.empty()) {
            auto& last = m_entries.back();
            if (last.level == level && last.message == normalized) {
                last.repeatCount++;
                return;
            }
        }

        LogEntry e{};
        e.message = std::move(normalized);
        e.level   = level;
        e.timeMs  = NowMsLocked();
        m_entries.push_back(std::move(e));

        if (m_entries.size() > MAX_LINES) {
            m_entries.pop_front();
        }
        if (!m_paused) {
            m_scrollToBottom = true;
        }
    }

    void InitStartTimeIfNeeded() {
        if (!m_startTimeInited) {
            m_startTime       = std::chrono::steady_clock::now();
            m_startTimeInited = true;
        }
    }

    uint64_t NowMsLocked() const {
        if (!m_startTimeInited) {
            return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_startTime).count());
    }

    static LogLevel DetectLevel(const std::string& msg) {
        // Info -> Warn
        if (msg.find("[WARN]") != std::string::npos || msg.find("[Warning]") != std::string::npos ||
            msg.find("warning") != std::string::npos || msg.find("Warning") != std::string::npos) {
            return LogLevel::Warn;
        }
        // Info/Warn -> Error
        if (msg.find("[ERROR]") != std::string::npos || msg.find("[Error]") != std::string::npos ||
            msg.find("error") != std::string::npos || msg.find("Error") != std::string::npos ||
            msg.find("failed") != std::string::npos || msg.find("Failed") != std::string::npos ||
            msg.find("FATAL") != std::string::npos || msg.find("Fatal") != std::string::npos) {
            return LogLevel::Error;
        }
        return LogLevel::Info;
    }

    static void AppendLine(std::string& out, const LogEntry& e) {
        out += e.message;
        if (e.repeatCount > 1) {
            out += " (x";
            out += std::to_string(e.repeatCount);
            out += ")";
        }
        out += "\n";
    }

    std::deque<LogEntry>          m_entries;
    std::mutex                    m_mutex;
    bool                          m_scrollToBottom = false;
    bool                          m_paused         = false;
    std::unordered_set<std::string> m_onceKeys;

    bool                              m_startTimeInited = false;
    std::chrono::steady_clock::time_point m_startTime{};

    static const size_t MAX_LINES = 2000;
};

// 重定向 std::cout/std::cerr 到调试日志
class DebugStreamBuf : public std::streambuf {
  public:
    explicit DebugStreamBuf(std::streambuf* orig, LogLevel defaultLevel = LogLevel::Info)
        : m_orig(orig), m_defaultLevel(defaultLevel) {}

    ~DebugStreamBuf() override {
        FlushPending();
    }

  protected:
    int overflow(int c) override {
        if (c == EOF) {
            return c;
        }

        if (c == '\n') {
            FlushPending();
        } else if (c != '\r') {
            m_buffer += static_cast<char>(c);
        }

        if (m_orig) {
            m_orig->sputc(c);
        }
        return c;
    }

    int sync() override {
        FlushPending();
        return 0;
    }

  private:
    void FlushPending() {
        if (!m_buffer.empty()) {
            DebugLog::Instance().AddFromStream(m_defaultLevel, m_buffer);
            m_buffer.clear();
        }
    }

    std::streambuf* m_orig         = nullptr;
    LogLevel        m_defaultLevel = LogLevel::Info;
    std::string     m_buffer;
};
