#pragma once
#include "stdafx.h"

struct sapi_voice_info {
    std::wstring id;
    std::wstring name;
};

std::vector<sapi_voice_info> sapi_enumerate_voices(const wchar_t* voiceType);
bool sapi_speak(const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt);
void sapi_queue_speak(const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt);
void sapi_queue_silence();
void sapi_shutdown();
