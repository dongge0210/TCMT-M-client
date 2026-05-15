#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>

namespace tcmt {

// Log ring buffer for TUI log panel
class LogBuffer {
public:
    static constexpr size_t MAX_LINES = 500;

    void Push(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
        while (lines_.size() > MAX_LINES) {
            lines_.pop_front();
        }
    }

    std::vector<std::string> GetRecent(size_t count) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> result;
        size_t start = (lines_.size() > count) ? lines_.size() - count : 0;
        for (size_t i = start; i < lines_.size(); ++i) {
            result.push_back(lines_[i]);
        }
        return result;
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_.size();
    }

private:
    mutable std::mutex mutex_;
    std::deque<std::string> lines_;
};

} // namespace tcmt
