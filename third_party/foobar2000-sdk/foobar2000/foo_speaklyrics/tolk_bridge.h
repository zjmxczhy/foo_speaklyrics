#pragma once
#include "stdafx.h"
#include "speech_task.h"

bool tolk_speak(const wchar_t* text, bool interrupt);
void tolk_silence();
speech_enqueue_result tolk_queue_task(speech_task task);
void tolk_invalidate_pending(uint64_t activeGeneration, speech_invalidation_reason reason);
void tolk_shutdown();
