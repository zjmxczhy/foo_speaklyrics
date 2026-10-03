#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// This sink has no SDK dependency. Its caller serializes access; diagnostics are
// returned rather than logged recursively or passed to the console under a lock.
namespace speaklyrics_log_io {

constexpr uint64_t default_max_bytes = 8ULL * 1024 * 1024;
constexpr uint64_t diagnostic_interval_ms = 30000;

enum class operation { inspect, rotate, open, write, close, count };
enum class notice_kind { failure, recovered, rotated, writing_resumed };

struct notice {
    notice_kind kind = notice_kind::failure;
    operation action = operation::write;
    DWORD error = ERROR_SUCCESS;
    uint64_t occurrences = 0;
    uint64_t expected_bytes = 0;
    uint64_t actual_bytes = 0;
    uint64_t file_bytes = 0;
};

struct append_result {
    bool success = false;
    std::vector<notice> notices;
};

struct file_api {
    decltype(&GetFileAttributesExW) inspect = &GetFileAttributesExW;
    decltype(&MoveFileExW) move = &MoveFileExW;
    decltype(&CreateFileW) open = &CreateFileW;
    decltype(&WriteFile) write = &WriteFile;
    decltype(&CloseHandle) close = &CloseHandle;
};

class file_handle {
public:
    file_handle(HANDLE handle, const file_api& api) : handle_(handle), api_(api) {}
    ~file_handle() { if (handle_ != INVALID_HANDLE_VALUE) api_.close(handle_); }
    file_handle(const file_handle&) = delete;
    file_handle& operator=(const file_handle&) = delete;
    bool close(DWORD& error) {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        if (api_.close(handle)) return true;
        error = GetLastError();
        return false;
    }
private:
    HANDLE handle_;
    const file_api& api_;
};

class sink {
public:
    explicit sink(uint64_t maxBytes = default_max_bytes, file_api api = {})
        : max_bytes_(maxBytes), api_(api) {}

    append_result append(const std::wstring& path, const std::string& bytes,
        uint64_t now) {
        append_result result;
        result.notices.reserve(12);
        if (path != path_) {
            path_ = path;
            failures_ = {};
            rotation_deferred_ = false;
            lost_records_ = 0;
        }
        rotate_if_needed(path, bytes.size(), now, result);
        const HANDLE raw = api_.open(path.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            failure(operation::open, GetLastError(), now, result);
            ++lost_records_;
            return result;
        }
        file_handle handle(raw, api_);
        recovered(operation::open, result);

        size_t offset = 0;
        bool writtenSuccessfully = true;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset,
                static_cast<size_t>(MAXDWORD)));
            DWORD written = 0;
            const BOOL ok = api_.write(raw, bytes.data() + offset, chunk,
                &written, nullptr);
            const DWORD error = ok ? ERROR_WRITE_FAULT : GetLastError();
            if (!ok || written == 0 || written > chunk) {
                failure(operation::write, error, now, result,
                    bytes.size(), offset + (std::min)(written, chunk));
                writtenSuccessfully = false;
                break;
            }
            offset += written; // A successful short write must be completed.
        }
        if (writtenSuccessfully) recovered(operation::write, result);

        DWORD closeError = ERROR_SUCCESS;
        const bool closed = handle.close(closeError);
        if (!closed) failure(operation::close, closeError, now, result);
        else recovered(operation::close, result);
        result.success = writtenSuccessfully && closed;
        if (!result.success) {
            ++lost_records_;
        } else if (lost_records_ != 0) {
            notice resumed;
            resumed.kind = notice_kind::writing_resumed;
            resumed.occurrences = lost_records_;
            result.notices.push_back(resumed);
            lost_records_ = 0;
        }
        return result;
    }

private:
    struct failure_state {
        bool active = false;
        DWORD error = ERROR_SUCCESS;
        uint64_t count = 0;
        uint64_t last_reported = 0;
    };

    void failure(operation action, DWORD error, uint64_t now,
        append_result& result, uint64_t expected = 0, uint64_t actual = 0, uint64_t fileBytes = 0) {
        auto& state = failures_[static_cast<size_t>(action)];
        const bool report = !state.active || state.error != error ||
            now - state.last_reported >= diagnostic_interval_ms;
        ++state.count;
        state.active = true;
        state.error = error;
        if (!report) return;
        state.last_reported = now;
        result.notices.push_back({notice_kind::failure, action, error,
            state.count, expected, actual, fileBytes});
    }

    void recovered(operation action, append_result& result) {
        auto& state = failures_[static_cast<size_t>(action)];
        if (!state.active) return;
        result.notices.push_back({notice_kind::recovered, action, state.error,
            state.count, 0, 0, 0});
        state = {};
    }

    void rotate_if_needed(const std::wstring& path, size_t recordBytes,
        uint64_t now, append_result& result) {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!api_.inspect(path.c_str(), GetFileExInfoStandard, &data)) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return;
            failure(operation::inspect, error, now, result);
            return; // Appending may still be possible even if inspection fails.
        }
        recovered(operation::inspect, result);
        const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
            data.nFileSizeLow;
        if (size == 0 || (size < max_bytes_ && recordBytes <= max_bytes_ - size)) return;
        if (rotation_deferred_ && now - last_rotation_attempt_ < diagnostic_interval_ms) return;
        last_rotation_attempt_ = now;
        // Replace the previous backup in one operation. Do not delete it first:
        // a failed rename must leave both the active log and old backup intact.
        const std::wstring backup = path + L".old";
        if (!api_.move(path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            rotation_deferred_ = true;
            failure(operation::rotate, GetLastError(), now, result, 0, 0, size);
            return;
        }
        rotation_deferred_ = false;
        recovered(operation::rotate, result);
        result.notices.push_back({notice_kind::rotated, operation::rotate,
            ERROR_SUCCESS, 0, 0, 0, size});
    }

    uint64_t max_bytes_;
    file_api api_;
    std::wstring path_;
    std::array<failure_state, static_cast<size_t>(operation::count)> failures_{};
    uint64_t last_rotation_attempt_ = 0;
    bool rotation_deferred_ = false;
    uint64_t lost_records_ = 0;
};

} // namespace speaklyrics_log_io
