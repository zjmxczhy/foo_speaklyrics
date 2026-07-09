#pragma once
#include "stdafx.h"

enum class lyric_copy_mode {
    ask = 0,
    timestamps,
    plain,
    split,
};

const char* lyric_copy_mode_to_id(lyric_copy_mode mode);
lyric_copy_mode lyric_copy_mode_from_id(const char* id);
const wchar_t* lyric_copy_mode_display_name(lyric_copy_mode mode);
bool copy_current_lyrics(lyric_copy_mode mode);
bool show_copy_lyrics_menu(HWND parent);
