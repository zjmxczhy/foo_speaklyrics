#include "stdafx.h"
#include "speech_engine.h"

#include "config.h"
#include "sapi_speech.h"
#include "tolk_bridge.h"

static std::wstring cfg_to_wide_speech(cfg_var_modern::cfg_string& s) {
    pfc::string8 v = s.get();
    return pfc::stringcvt::string_wide_from_utf8(v).get_ptr();
}

bool speech_speak(const wchar_t* text, bool interrupt) {
    if (!cfg_use_screen_reader.get()) {
        std::wstring voiceId = cfg_to_wide_speech(cfg_tts_voice_id);
        return sapi_speak(L"sapi5", voiceId.c_str(), static_cast<int>(cfg_tts_rate.get()), text, interrupt);
    }
    return tolk_speak(text, interrupt);
}

void speech_queue_speak(const wchar_t* text, bool interrupt) {
    if (!cfg_use_screen_reader.get()) {
        std::wstring voiceId = cfg_to_wide_speech(cfg_tts_voice_id);
        sapi_queue_speak(L"sapi5", voiceId.c_str(), static_cast<int>(cfg_tts_rate.get()), text, interrupt);
        return;
    }
    tolk_queue_speak(text, interrupt);
}

void speech_queue_silence() {
    if (!cfg_use_screen_reader.get()) {
        sapi_queue_silence();
        return;
    }
    tolk_queue_silence();
}

void speech_shutdown() {
    sapi_shutdown();
    tolk_shutdown();
}
