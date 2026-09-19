#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

enum class lrc_download_status {
    idle,
    in_progress,
    succeeded,
    transient_failure,
    not_found,
    configuration_unavailable,
};

namespace lrc_download_retry_policy {

inline constexpr uint64_t no_retry_tick = (std::numeric_limits<uint64_t>::max)();
inline constexpr uint64_t not_found_delay_ms = 10ULL * 60ULL * 1000ULL;
inline constexpr uint64_t missing_downloader_delay_ms = 5ULL * 60ULL * 1000ULL;

inline uint64_t add_delay(uint64_t now, uint64_t delay) {
    if (delay >= no_retry_tick - now) return no_retry_tick;
    return now + delay;
}

inline uint64_t transient_delay_ms(uint32_t failure_number) {
    static constexpr uint64_t delays[] = {
        5000ULL,
        15000ULL,
        30000ULL,
        60000ULL,
        120000ULL,
        300000ULL,
    };
    if (failure_number == 0) failure_number = 1;
    const size_t index = failure_number > sizeof(delays) / sizeof(delays[0])
        ? sizeof(delays) / sizeof(delays[0]) - 1
        : static_cast<size_t>(failure_number - 1);
    return delays[index];
}

} // namespace lrc_download_retry_policy

struct lrc_download_retry_state {
    std::wstring track_key;
    lrc_download_status status = lrc_download_status::idle;
    uint32_t attempt_count = 0;
    uint32_t transient_failure_count = 0;
    uint64_t next_allowed_tick = 0;
    int last_process_status = -1;
    uint32_t last_exit_code = 0;
    uint32_t last_error_code = 0;
    std::wstring last_reason;

    void reset() {
        track_key.clear();
        status = lrc_download_status::idle;
        attempt_count = 0;
        transient_failure_count = 0;
        next_allowed_tick = 0;
        last_process_status = -1;
        last_exit_code = 0;
        last_error_code = 0;
        last_reason.clear();
    }

    bool is_for(const std::wstring& key) const {
        return !key.empty() && track_key == key;
    }

    bool in_progress_for(const std::wstring& key) const {
        return is_for(key) && status == lrc_download_status::in_progress;
    }

    bool can_attempt(const std::wstring& key, uint64_t now) const {
        if (key.empty()) return false;
        if (!is_for(key)) return true;

        switch (status) {
        case lrc_download_status::idle:
            return true;
        case lrc_download_status::transient_failure:
        case lrc_download_status::not_found:
        case lrc_download_status::configuration_unavailable:
            return next_allowed_tick != lrc_download_retry_policy::no_retry_tick &&
                now >= next_allowed_tick;
        case lrc_download_status::in_progress:
        case lrc_download_status::succeeded:
        default:
            return false;
        }
    }

    uint32_t begin_attempt(const std::wstring& key) {
        ensure_track(key);
        status = lrc_download_status::in_progress;
        next_allowed_tick = 0;
        last_process_status = -1;
        last_exit_code = 0;
        last_error_code = 0;
        last_reason.clear();
        return ++attempt_count;
    }

    uint64_t mark_transient_failure(const std::wstring& key, uint64_t now,
        const std::wstring& reason, int process_status = -1,
        uint32_t exit_code = 0, uint32_t error_code = 0) {
        ensure_track(key);
        status = lrc_download_status::transient_failure;
        last_process_status = process_status;
        last_exit_code = exit_code;
        last_error_code = error_code;
        last_reason = reason;
        const uint64_t delay = lrc_download_retry_policy::transient_delay_ms(
            ++transient_failure_count);
        next_allowed_tick = lrc_download_retry_policy::add_delay(now, delay);
        return delay;
    }

    uint64_t mark_not_found(const std::wstring& key, uint64_t now,
        const std::wstring& reason, uint32_t exit_code = 1) {
        ensure_track(key);
        status = lrc_download_status::not_found;
        transient_failure_count = 0;
        last_process_status = -1;
        last_exit_code = exit_code;
        last_error_code = 0;
        last_reason = reason;
        next_allowed_tick = lrc_download_retry_policy::add_delay(
            now, lrc_download_retry_policy::not_found_delay_ms);
        return lrc_download_retry_policy::not_found_delay_ms;
    }

    void mark_configuration_unavailable(const std::wstring& key, uint64_t now,
        const std::wstring& reason,
        uint64_t retry_delay_ms = lrc_download_retry_policy::no_retry_tick,
        uint32_t exit_code = 0, uint32_t error_code = 0) {
        ensure_track(key);
        status = lrc_download_status::configuration_unavailable;
        transient_failure_count = 0;
        last_process_status = -1;
        last_exit_code = exit_code;
        last_error_code = error_code;
        last_reason = reason;
        next_allowed_tick = retry_delay_ms == lrc_download_retry_policy::no_retry_tick
            ? lrc_download_retry_policy::no_retry_tick
            : lrc_download_retry_policy::add_delay(now, retry_delay_ms);
    }

    void mark_succeeded(const std::wstring& key) {
        ensure_track(key);
        status = lrc_download_status::succeeded;
        transient_failure_count = 0;
        next_allowed_tick = 0;
        last_process_status = -1;
        last_exit_code = 0;
        last_error_code = 0;
        last_reason.clear();
    }

    uint64_t retry_delay_remaining(uint64_t now) const {
        if (next_allowed_tick == lrc_download_retry_policy::no_retry_tick) {
            return lrc_download_retry_policy::no_retry_tick;
        }
        return next_allowed_tick > now ? next_allowed_tick - now : 0;
    }

private:
    void ensure_track(const std::wstring& key) {
        if (track_key == key) return;
        reset();
        track_key = key;
    }
};
