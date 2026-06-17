#pragma once
#include "stdafx.h"

void tolk_start_worker();
bool tolk_speak(const wchar_t* text, bool interrupt);
void tolk_silence();
void tolk_queue_speak(const wchar_t* text, bool interrupt);
void tolk_queue_silence();
void tolk_shutdown();
