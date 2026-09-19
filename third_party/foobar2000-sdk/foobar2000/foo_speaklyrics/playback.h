#pragma once

void reload_current_lyrics();

enum class ensure_current_lyrics_result {
    already_loaded,
    loaded,
    no_playing_track,
    background_download_pending,
    unavailable,
};

ensure_current_lyrics_result ensure_current_lyrics_loaded_for_copy();

void cancel_playback_background_tasks();

void set_manual_lrc_file_for_current_track(const char* path);

void bind_manual_lrc_to_current_track();

bool copy_current_lyrics_without_timestamps();

bool copy_text_to_clipboard(const std::wstring& text);

bool speak_current_track_announcement();

bool switch_to_next_same_title_lyrics();
bool switch_to_previous_same_title_lyrics();

struct lyric_jump_item {
    int time_ms = 0;
    std::wstring text;
    uint64_t line_id = 0;
    size_t document_index = 0;
};

std::vector<lyric_jump_item> get_current_lyric_jump_items();

#include "lyric_scheduler.h"

std::vector<lyric_schedule_item> get_current_lyric_schedule_items();

bool jump_to_lyric_time_ms(int time_ms);

struct current_track_search_info {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int duration_seconds = 0;
};

current_track_search_info get_current_track_search_info();
