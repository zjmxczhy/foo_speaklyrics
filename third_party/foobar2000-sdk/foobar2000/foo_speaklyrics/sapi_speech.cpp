#include "stdafx.h"
#include "sapi_speech.h"

#include <sapi.h>

namespace {

enum sapi_task_kind { sapi_task_none, sapi_task_speak, sapi_task_silence, sapi_task_quit };

CRITICAL_SECTION g_sapi_lock;
bool g_sapi_lock_ready = false;
HANDLE g_sapi_worker = nullptr;
HANDLE g_sapi_event = nullptr;
bool g_sapi_quit = false;
sapi_task_kind g_sapi_task = sapi_task_none;
std::wstring g_task_type;
std::wstring g_task_voice_id;
std::wstring g_task_text;
int g_task_rate = 0;
bool g_task_interrupt = true;

const wchar_t* category_for_type(const wchar_t* voiceType) {
    if (voiceType && _wcsicmp(voiceType, L"onecore") == 0) {
        return L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech_OneCore\\Voices";
    }
    return SPCAT_VOICES;
}

IEnumSpObjectTokens* enum_voice_tokens(const wchar_t* voiceType) {
    ISpObjectTokenCategory* category = nullptr;
    IEnumSpObjectTokens* tokens = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL, IID_ISpObjectTokenCategory, reinterpret_cast<void**>(&category));
    if (SUCCEEDED(hr) && category) {
        hr = category->SetId(category_for_type(voiceType), FALSE);
        if (SUCCEEDED(hr)) category->EnumTokens(nullptr, nullptr, &tokens);
        category->Release();
    }
    return tokens;
}

ISpObjectToken* token_from_id(const wchar_t* tokenId) {
    if (!tokenId || !*tokenId) return nullptr;
    ISpObjectToken* token = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SpObjectToken, nullptr, CLSCTX_ALL, IID_ISpObjectToken, reinterpret_cast<void**>(&token));
    if (SUCCEEDED(hr) && token && SUCCEEDED(token->SetId(nullptr, tokenId, FALSE))) return token;
    if (token) token->Release();
    return nullptr;
}

void init_lock_once() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&g_sapi_lock);
        g_sapi_lock_ready = true;
        return TRUE;
    }, nullptr, nullptr);
}

std::wstring token_description(ISpObjectToken* token) {
    if (!token) return L"";
    wchar_t* desc = nullptr;
    HRESULT hr = token->GetStringValue(nullptr, &desc);
    std::wstring out;
    if (SUCCEEDED(hr) && desc) out = desc;
    if (desc) CoTaskMemFree(desc);
    return out;
}

std::wstring token_id(ISpObjectToken* token) {
    if (!token) return L"";
    wchar_t* id = nullptr;
    HRESULT hr = token->GetId(&id);
    std::wstring out;
    if (SUCCEEDED(hr) && id) out = id;
    if (id) CoTaskMemFree(id);
    return out;
}

ISpObjectToken* find_voice_token(const wchar_t* voiceType, const wchar_t* voiceId) {
    ISpObjectToken* token = token_from_id(voiceId);
    if (token) return token;

    IEnumSpObjectTokens* tokens = enum_voice_tokens(voiceType);
    ULONG count = 0;
    if (tokens) {
        if (SUCCEEDED(tokens->GetCount(&count)) && count > 0) tokens->Item(0, &token);
        tokens->Release();
    }
    return token;
}

bool configure_voice_unsafe(ISpVoice* voice, const wchar_t* voiceType, const wchar_t* voiceId, int rate) {
    if (!voice) return false;

    ISpObjectToken* token = find_voice_token(voiceType, voiceId);
    if (token) {
        // Ignore bad third-party voice tokens instead of letting one broken voice block all TTS.
        voice->SetVoice(token);
        token->Release();
    }

    if (rate < -10) rate = -10;
    if (rate > 10) rate = 10;
    voice->SetRate(rate);
    return true;
}

bool speak_with_voice_unsafe(ISpVoice* voice, const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    if (!voice) return false;
    if (!text || !*text) return true;

    configure_voice_unsafe(voice, voiceType, voiceId, rate);

    DWORD flags = SPF_IS_NOT_XML | SPF_ASYNC;
    if (interrupt) flags |= SPF_PURGEBEFORESPEAK;
    HRESULT hr = voice->Speak(text, flags, nullptr);
    return SUCCEEDED(hr);
}

void purge_sapi_queue_unsafe(ISpVoice* voice) {
    if (voice) voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
}

bool speak_with_voice(ISpVoice* voice, const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    __try {
        return speak_with_voice_unsafe(voice, voiceType, voiceId, rate, text, interrupt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void purge_sapi_queue(ISpVoice* voice) {
    __try {
        purge_sapi_queue_unsafe(voice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

DWORD WINAPI sapi_worker_proc(void*) {
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    ISpVoice* voice = nullptr;
    HRESULT hrVoice = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(&voice));
    if (FAILED(hrVoice)) voice = nullptr;

    for (;;) {
        WaitForSingleObject(g_sapi_event, INFINITE);

        sapi_task_kind kind = sapi_task_none;
        std::wstring type, voiceId, text;
        int rate = 0;
        bool interrupt = true;
        EnterCriticalSection(&g_sapi_lock);
        kind = g_sapi_task;
        type.swap(g_task_type);
        voiceId.swap(g_task_voice_id);
        text.swap(g_task_text);
        rate = g_task_rate;
        interrupt = g_task_interrupt;
        g_sapi_task = sapi_task_none;
        bool quit = g_sapi_quit || kind == sapi_task_quit;
        LeaveCriticalSection(&g_sapi_lock);

        if (quit) break;
        if (kind == sapi_task_silence) {
            purge_sapi_queue(voice);
        } else if (kind == sapi_task_speak && !text.empty()) {
            speak_with_voice(voice, type.c_str(), voiceId.c_str(), rate, text.c_str(), interrupt);
        }
    }

    purge_sapi_queue(voice);
    if (voice) voice->Release();
    if (coInitialized) CoUninitialize();
    return 0;
}

bool ensure_worker() {
    init_lock_once();
    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_worker) {
        LeaveCriticalSection(&g_sapi_lock);
        return true;
    }
    g_sapi_quit = false;
    g_sapi_task = sapi_task_none;
    g_sapi_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_sapi_event) {
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }
    g_sapi_worker = CreateThread(nullptr, 0, sapi_worker_proc, nullptr, 0, nullptr);
    if (!g_sapi_worker) {
        CloseHandle(g_sapi_event);
        g_sapi_event = nullptr;
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }
    LeaveCriticalSection(&g_sapi_lock);
    return true;
}

void post_task(sapi_task_kind kind, const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    if (!ensure_worker()) return;
    EnterCriticalSection(&g_sapi_lock);
    g_sapi_task = kind;
    g_task_type = voiceType ? voiceType : L"sapi5";
    g_task_voice_id = voiceId ? voiceId : L"";
    g_task_text = text ? text : L"";
    g_task_rate = rate;
    g_task_interrupt = interrupt;
    HANDLE event = g_sapi_event;
    LeaveCriticalSection(&g_sapi_lock);
    if (event) SetEvent(event);
}

} // namespace

std::vector<sapi_voice_info> sapi_enumerate_voices(const wchar_t* voiceType) {
    std::vector<sapi_voice_info> out;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    IEnumSpObjectTokens* tokens = nullptr;
    ULONG count = 0;
    tokens = enum_voice_tokens(voiceType);
    if (tokens) {
        if (SUCCEEDED(tokens->GetCount(&count))) {
            for (ULONG i = 0; i < count; ++i) {
                ISpObjectToken* token = nullptr;
                if (SUCCEEDED(tokens->Item(i, &token)) && token) {
                    sapi_voice_info info;
                    info.id = token_id(token);
                    info.name = token_description(token);
                    if (info.name.empty()) info.name = info.id;
                    if (!info.id.empty()) out.push_back(info);
                    token->Release();
                }
            }
        }
        tokens->Release();
    }
    if (coInitialized) CoUninitialize();
    return out;
}

bool sapi_speak(const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    if (!text || !*text) return true;
    if (!ensure_worker()) return false;
    post_task(sapi_task_speak, voiceType, voiceId, rate, text, interrupt);
    return true;
}

void sapi_queue_speak(const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    if (!text || !*text) return;
    post_task(sapi_task_speak, voiceType, voiceId, rate, text, interrupt);
}

void sapi_queue_silence() {
    post_task(sapi_task_silence, L"sapi5", L"", 0, L"", true);
}

void sapi_shutdown() {
    init_lock_once();
    HANDLE worker = nullptr;
    HANDLE event = nullptr;
    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_worker) {
        g_sapi_quit = true;
        g_sapi_task = sapi_task_quit;
        worker = g_sapi_worker;
        event = g_sapi_event;
    }
    LeaveCriticalSection(&g_sapi_lock);

    if (event) SetEvent(event);
    if (worker) WaitForSingleObject(worker, 5000);

    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_worker) {
        CloseHandle(g_sapi_worker);
        g_sapi_worker = nullptr;
    }
    if (g_sapi_event) {
        CloseHandle(g_sapi_event);
        g_sapi_event = nullptr;
    }
    g_sapi_quit = false;
    g_sapi_task = sapi_task_none;
    g_task_type.clear();
    g_task_voice_id.clear();
    g_task_text.clear();
    LeaveCriticalSection(&g_sapi_lock);
}
