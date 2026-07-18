#pragma once
#include "stdafx.h"

enum class tolk_speech_task_type {
    general,
    lyric,
    auto_speak_enabled,
    auto_speak_disabled,
};

void tolk_preload();
bool tolk_speak(const wchar_t* text, bool interrupt);
void tolk_silence();
void tolk_queue_speak(const wchar_t* text, bool interrupt);
void tolk_queue_speak_task(const wchar_t* text, bool interrupt, tolk_speech_task_type type, unsigned retryWindowMs);
void tolk_queue_silence();
void tolk_invalidate_pending();
void tolk_shutdown();
