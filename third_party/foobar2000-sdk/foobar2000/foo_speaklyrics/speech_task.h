#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class speech_task_type {
    preload,
    general,
    track_announcement,
    lyric,
    auto_speak_enabled,
    auto_speak_disabled,
};

enum class speech_backend_type {
    tolk,
    sapi,
};

enum class speech_enqueue_status {
    accepted,
    invalid_input,
    expired,
    stale_generation,
    worker_unavailable,
    shutting_down,
    queue_full,
    signal_failed,
};

enum class speech_task_result_status {
    dispatched,
    failed,
    expired,
    canceled,
};

enum class speech_invalidation_reason {
    none,
    track_change,
    playback_stop,
    playback_pause,
    playback_seek,
    auto_speak_disabled,
    lyrics_reload,
    metadata_change,
    same_title_lyrics,
    silence,
    shutdown,
    backend_switch,
    superseded_by_lyric,
    queue_pressure,
    worker_exit,
};

struct speech_task {
    uint64_t task_id = 0;
    speech_task_type type = speech_task_type::general;
    std::wstring text;
    bool interrupt = true;

    uint64_t track_session = 0;
    uint64_t document_generation = 0;
    uint64_t playback_generation = 0;
    int line_index = -1;
    uint64_t line_id = 0;
    std::vector<uint64_t> line_ids;
    uint64_t position_epoch = 0;
    int lyric_time_ms = 0;
    int trigger_time_ms = 0;
    int playback_time_ms = 0;
    uint64_t text_hash = 0;

    uint64_t queued_at = 0;
    uint64_t ready_at = 0;
    uint64_t expires_at = 0;
    unsigned retry_attempt = 0;

    std::wstring voice_type;
    std::wstring voice_id;
    int voice_rate = 0;
};

struct speech_enqueue_result {
    speech_enqueue_status status = speech_enqueue_status::invalid_input;
    uint64_t task_id = 0;
    uint64_t playback_generation = 0;
    uint64_t expires_at = 0;
    size_t queue_depth = 0;

    bool accepted() const { return status == speech_enqueue_status::accepted; }
};

struct speech_task_result {
    speech_backend_type backend = speech_backend_type::tolk;
    speech_task_result_status status = speech_task_result_status::failed;
    speech_invalidation_reason reason = speech_invalidation_reason::none;
    speech_task task;
    int64_t error_code = 0;
    uint64_t completed_at = 0;
};

struct speech_task_cancellation {
    speech_task task;
    speech_invalidation_reason reason = speech_invalidation_reason::none;
};

inline bool speech_task_is_lyric(const speech_task& task) {
    return task.type == speech_task_type::lyric;
}

inline bool speech_task_is_expired(const speech_task& task, uint64_t now) {
    return task.expires_at != 0 && now >= task.expires_at;
}

inline bool speech_task_can_retry(const speech_task& task) {
    return task.type == speech_task_type::preload ||
        task.type == speech_task_type::lyric ||
        task.type == speech_task_type::auto_speak_enabled ||
        task.type == speech_task_type::auto_speak_disabled;
}
