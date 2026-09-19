#pragma once
#include "stdafx.h"
#include "speech_task.h"

struct lyric_speech_diagnostic_context {
    uint64_t track_session = 0;
    uint64_t document_generation = 0;
    int line_index = -1;
    uint64_t line_id = 0;
    std::vector<uint64_t> line_ids;
    uint64_t position_epoch = 0;
    int lyric_time_ms = 0;
    int trigger_time_ms = 0;
    int playback_time_ms = 0;
};

bool speech_speak(const wchar_t* text, bool interrupt);
void speech_preload();
void speech_queue_speak(const wchar_t* text, bool interrupt);
void speech_queue_track_announcement(const wchar_t* text, bool interrupt);
speech_enqueue_result speech_queue_lyric(const wchar_t* text, bool interrupt, unsigned validMs,
    const lyric_speech_diagnostic_context& diagnostic);
void speech_queue_auto_speak_state(bool enabled);
void speech_queue_silence();
uint64_t speech_allocate_task_id();
uint64_t speech_invalidate_pending(speech_invalidation_reason reason);
uint64_t speech_current_playback_generation();
void speech_report_task_result(speech_task_result result);
std::vector<speech_task_result> speech_drain_task_results();
void speech_shutdown();
