#pragma once
#include "stdafx.h"

bool speech_speak(const wchar_t* text, bool interrupt);
void speech_queue_speak(const wchar_t* text, bool interrupt);
void speech_queue_silence();
void speech_shutdown();
