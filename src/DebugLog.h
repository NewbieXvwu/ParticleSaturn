#pragma once
// 调试日志系统 - 带 ImGui 显示的日志记录

#include <deque>
#include <mutex>
#include <streambuf>

class DebugLog {
  public:
    static DebugLog& Instance() {
        static DebugLog inst;
        return inst;
    }

    void Add(const std::string& msg) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back(msg);
        if (m_lines.size() > MAX_LINES) {
            m_lines.pop_front();
        }
        m_scrollToBottom = true;
    }

    void Draw() {
        ImGui::BeginChild("LogScroll", ImVec2(0, 200), true);
        for (const auto& line : m_lines) {
            ImGui::TextUnformatted(line.c_str());
        }
        if (m_scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            m_scrollToBottom = false;
        }
        ImGui::EndChild();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.clear();
    }

    std::string GetAllText() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string                 result;
        for (const auto& line : m_lines) {
            result += line + "\n";
        }
        return result;
    }

  private:
    DebugLog() = default;
    std::deque<std::string> m_lines;
    std::mutex              m_mutex;
    bool                    m_scrollToBottom = false;
    static const size_t     MAX_LINES        = 200;
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
