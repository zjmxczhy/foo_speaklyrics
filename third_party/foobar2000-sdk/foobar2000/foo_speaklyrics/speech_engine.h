#pragma once
#include "stdafx.h"

bool speech_speak(const wchar_t* text, bool interrupt);
void speech_preload();
void speech_queue_speak(const wchar_t* text, bool interrupt);
void speech_queue_lyric(const wchar_t* text, bool interrupt, unsigned validMs);
void speech_queue_auto_speak_state(bool enabled);
void speech_queue_silence();
void speech_invalidate_pending();
void speech_shutdown();
