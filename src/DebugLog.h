#pragma once
// 调试日志系统 - 带 ImGui 显示的日志记录

#include <imgui.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <streambuf>
#include <string>

#include "md3/MD3.h"

// 日志级别
enum class LogLevel {
    Info,
    Warn,
    Error
};

// 日志条目
struct LogEntry {
    std::string message;
    LogLevel    level;
};

class DebugLog {
  public:
    static DebugLog& Instance() {
        static DebugLog inst;
        return inst;
    }

    void Add(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // 自动检测日志级别
        LogLevel level = LogLevel::Info;
        if (msg.find("[WARN]") != std::string::npos || msg.find("[Warning]") != std::string::npos ||
            msg.find("warning") != std::string::npos || msg.find("Warning") != std::string::npos) {
            level = LogLevel::Warn;
        } else if (msg.find("[ERROR]") != std::string::npos || msg.find("[Error]") != std::string::npos ||
                   msg.find("error") != std::string::npos || msg.find("Error") != std::string::npos ||
                   msg.find("failed") != std::string::npos || msg.find("Failed") != std::string::npos) {
            level = LogLevel::Error;
        }

        m_entries.push_back({msg, level});
        if (m_entries.size() > MAX_LINES) {
            m_entries.pop_front();
        }
        if (!m_paused) {
            m_scrollToBottom = true;
        }
    }

    // 绘制日志（带过滤和搜索）
    void Draw(const char* searchFilter, int levelFilter) {
        ImGui::BeginChild("LogScroll", ImVec2(0, 200), true);

        std::lock_guard<std::mutex> lock(m_mutex);
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

            // 搜索过滤
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

            ImGui::PushStyleColor(ImGuiCol_Text, color);

            // 高亮搜索关键字
            if (searchFilter && searchFilter[0] != '\0') {
                const std::string& text      = entry.message;
                size_t             pos       = 0;
                size_t             searchLen = strlen(searchFilter);
                size_t             lastPos   = 0;

                while ((pos = text.find(searchFilter, lastPos)) != std::string::npos) {
                    // 输出匹配前的文本
                    if (pos > lastPos) {
                        ImGui::TextUnformatted(text.c_str() + lastPos, text.c_str() + pos);
                        ImGui::SameLine(0, 0);
                    }
                    // 高亮匹配文本
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                    ImGui::TextUnformatted(text.c_str() + pos, text.c_str() + pos + searchLen);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(0, 0);
                    lastPos = pos + searchLen;
                }
                // 输出剩余文本
                if (lastPos < text.length()) {
                    ImGui::TextUnformatted(text.c_str() + lastPos);
                } else {
                    ImGui::NewLine();
                }
            } else {
                ImGui::TextUnformatted(entry.message.c_str());
            }

            ImGui::PopStyleColor();
        }

        if (m_scrollToBottom && !m_paused) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }
        ImGui::EndChild();
    }

    // 保持向后兼容的简单绘制
    void Draw() { Draw(nullptr, 0); }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
    }

    std::string GetAllText() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string                 result;
        for (const auto& entry : m_entries) {
            result += entry.message + "\n";
        }
        return result;
    }

    // 获取过滤后的文本（用于复制）
    std::string GetFilteredText(const char* searchFilter, int levelFilter) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string                 result;
        for (const auto& entry : m_entries) {
            // 级别过滤
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
            // 搜索过滤
            if (searchFilter && searchFilter[0] != '\0') {
                if (entry.message.find(searchFilter) == std::string::npos) {
                    continue;
                }
            }
            result += entry.message + "\n";
        }
        return result;
    }

    void SetPaused(bool paused) { m_paused = paused; }

    bool IsPaused() const { return m_paused; }

  private:
    DebugLog() = default;
    std::deque<LogEntry> m_entries;
    std::mutex           m_mutex;
    bool                 m_scrollToBottom = false;
    bool                 m_paused         = false;
    static const size_t  MAX_LINES        = 500; // 增加容量
};

// 重定向 std::cout 到调试日志
class DebugStreamBuf : public std::streambuf {
  public:
    DebugStreamBuf(std::streambuf* orig) : m_orig(orig) {}

  protected:
    int overflow(int c) override {
        if (c != EOF) {
            if (c == '\n') {
                DebugLog::Instance().Add(m_buffer);
                m_buffer.clear();
            } else {
                m_buffer += (char)c;
            }
            if (m_orig) {
                m_orig->sputc(c);
            }
        }
        return c;
    }

  private:
    std::streambuf* m_orig;
    std::string     m_buffer;
};
