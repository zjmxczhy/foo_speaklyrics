#pragma once
#include "stdafx.h"
#include "speech_task.h"

struct sapi_voice_info {
    std::wstring id;
    std::wstring name;
};

std::vector<sapi_voice_info> sapi_enumerate_voices(const wchar_t* voiceType);
bool sapi_speak(const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt);
speech_enqueue_result sapi_queue_task(speech_task task);
void sapi_invalidate_pending(uint64_t activeGeneration, speech_invalidation_reason reason);
void sapi_shutdown();
