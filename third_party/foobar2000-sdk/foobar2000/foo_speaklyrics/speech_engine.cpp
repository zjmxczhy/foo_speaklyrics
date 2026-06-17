#include "stdafx.h"
#include "speech_engine.h"

#include "tolk_bridge.h"

bool speech_speak(const wchar_t* text, bool interrupt) {
    return tolk_speak(text, interrupt);
}

void speech_queue_speak(const wchar_t* text, bool interrupt) {
    tolk_queue_speak(text, interrupt);
}

void speech_queue_silence() {
    tolk_queue_silence();
}

void speech_shutdown() {
    tolk_shutdown();
}
