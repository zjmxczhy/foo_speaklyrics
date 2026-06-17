#include "stdafx.h"
#include "tolk_bridge.h"

namespace {
using Tolk_Load_t = void(__cdecl*)();
using Tolk_Unload_t = void(__cdecl*)();
using Tolk_TrySAPI_t = void(__cdecl*)(bool);
using Tolk_Output_t = bool(__cdecl*)(const wchar_t*, bool);
using Tolk_Speak_t = bool(__cdecl*)(const wchar_t*, bool);
using Tolk_Silence_t = bool(__cdecl*)();

enum task_kind { task_none, task_speak, task_silence, task_quit };

HMODULE g_tolk = nullptr;
Tolk_Load_t pLoad = nullptr;
Tolk_Unload_t pUnload = nullptr;
Tolk_TrySAPI_t pTrySAPI = nullptr;
Tolk_Output_t pOutput = nullptr;
Tolk_Speak_t pSpeak = nullptr;
Tolk_Silence_t pSilence = nullptr;
bool g_loaded = false;
CRITICAL_SECTION g_tolk_lock;
CRITICAL_SECTION g_task_lock;
bool g_locks_ready = false;

HANDLE g_worker = nullptr;
HANDLE g_event = nullptr;
bool g_worker_quit = false;
task_kind g_task = task_none;
std::wstring g_task_text;
bool g_task_interrupt = true;
std::wstring g_component_dir;

void init_locks_once() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&g_tolk_lock);
        InitializeCriticalSection(&g_task_lock);
        g_locks_ready = true;
        return TRUE;
    }, nullptr, nullptr);
}


bool safe_try_sapi(Tolk_TrySAPI_t fn, bool enable) {
    if (!fn) return true;
    __try { fn(enable); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool safe_load(Tolk_Load_t fn) {
    if (!fn) return false;
    __try { fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template<typename Fn>
bool safe_text_call(Fn fn, const wchar_t* text, bool interrupt) {
    if (!fn) return false;
    __try { return fn(text, interrupt); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void safe_silence(Tolk_Silence_t fn) {
    if (!fn) return;
    __try { fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

std::wstring current_dll_dir() {
    pfc::string8 path = core_api::get_my_full_path();
    std::wstring wide = pfc::stringcvt::string_wide_from_utf8(path).get_ptr();
    size_t slash = wide.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : wide.substr(0, slash);
}

bool ensure_loaded() {
    if (g_loaded && (pSpeak || pOutput)) return true;

    if (g_component_dir.empty()) g_component_dir = current_dll_dir();

    std::wstring tolk_dir = g_component_dir;
    if (!tolk_dir.empty()) tolk_dir += L"\\tolk";

    // Tolk loads support DLLs such as ZDSRAPI_x64.dll and BoyCtrl-x64.dll
    // by file name, so include our bundled folder in DLL search.
    if (!tolk_dir.empty()) SetDllDirectoryW(tolk_dir.c_str());

    std::wstring path = tolk_dir;
    if (!path.empty()) path += L"\\Tolk.dll";
    g_tolk = LoadLibraryW(path.empty() ? L"Tolk.dll" : path.c_str());
    if (!g_tolk) g_tolk = LoadLibraryW(L"Tolk.dll");
    if (!g_tolk) return false;

    pLoad = reinterpret_cast<Tolk_Load_t>(GetProcAddress(g_tolk, "Tolk_Load"));
    pUnload = reinterpret_cast<Tolk_Unload_t>(GetProcAddress(g_tolk, "Tolk_Unload"));
    pTrySAPI = reinterpret_cast<Tolk_TrySAPI_t>(GetProcAddress(g_tolk, "Tolk_TrySAPI"));
    pOutput = reinterpret_cast<Tolk_Output_t>(GetProcAddress(g_tolk, "Tolk_Output"));
    pSpeak = reinterpret_cast<Tolk_Speak_t>(GetProcAddress(g_tolk, "Tolk_Speak"));
    pSilence = reinterpret_cast<Tolk_Silence_t>(GetProcAddress(g_tolk, "Tolk_Silence"));
    if (!pLoad || !pUnload || (!pSpeak && !pOutput)) {
        FreeLibrary(g_tolk);
        g_tolk = nullptr;
        pLoad = nullptr;
        pUnload = nullptr;
        pTrySAPI = nullptr;
        pOutput = nullptr;
        pSpeak = nullptr;
        pSilence = nullptr;
        return false;
    }
    safe_try_sapi(pTrySAPI, false);
    if (!safe_load(pLoad)) {
        // Avoid FreeLibrary after Tolk_Load may have partially run: some drivers
        // crash during unload. The process will reclaim the module on exit.
        pLoad = nullptr;
        pUnload = nullptr;
        pTrySAPI = nullptr;
        pOutput = nullptr;
        pSpeak = nullptr;
        pSilence = nullptr;
        return false;
    }
    g_loaded = true;
    return true;
}

bool speak_direct(const wchar_t* text, bool interrupt) {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    bool ok = true;
    if (text && *text) {
        ok = ensure_loaded();
        if (ok) {
            ok = safe_text_call(pSpeak, text, interrupt);
            if (!ok) ok = safe_text_call(pOutput, text, interrupt);
        }
    }
    LeaveCriticalSection(&g_tolk_lock);
    return ok;
}

void silence_direct() {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    if (g_loaded && pSilence) safe_silence(pSilence);
    LeaveCriticalSection(&g_tolk_lock);
}

void unload_direct() {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    // Do not call Tolk_Unload or FreeLibrary here. Some screen reader / SAPI
    // drivers crash while unloading; the OS will reclaim them when foobar2000 exits.
    if (g_loaded && pSilence) safe_silence(pSilence);
    LeaveCriticalSection(&g_tolk_lock);
}

DWORD WINAPI worker_proc(void*) {
    for (;;) {
        WaitForSingleObject(g_event, INFINITE);

        task_kind kind = task_none;
        std::wstring text;
        bool interrupt = true;
        EnterCriticalSection(&g_task_lock);
        kind = g_task;
        text.swap(g_task_text);
        interrupt = g_task_interrupt;
        g_task = task_none;
        bool quit = g_worker_quit || kind == task_quit;
        LeaveCriticalSection(&g_task_lock);

        if (quit) break;
        if (kind == task_silence) silence_direct();
        else if (kind == task_speak) speak_direct(text.c_str(), interrupt);
    }

    silence_direct();
    unload_direct();
    return 0;
}

bool ensure_worker() {
    init_locks_once();
    EnterCriticalSection(&g_task_lock);
    if (g_worker) {
        LeaveCriticalSection(&g_task_lock);
        return true;
    }
    if (g_component_dir.empty()) g_component_dir = current_dll_dir();
    g_worker_quit = false;
    g_task = task_none;
    g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_event) {
        LeaveCriticalSection(&g_task_lock);
        return false;
    }
    g_worker = CreateThread(nullptr, 0, worker_proc, nullptr, 0, nullptr);
    if (!g_worker) {
        CloseHandle(g_event);
        g_event = nullptr;
        LeaveCriticalSection(&g_task_lock);
        return false;
    }
    LeaveCriticalSection(&g_task_lock);
    return true;
}

void post_task(task_kind kind, const wchar_t* text, bool interrupt) {
    if (!ensure_worker()) return;
    EnterCriticalSection(&g_task_lock);
    g_task = kind;
    g_task_text = text ? text : L"";
    g_task_interrupt = interrupt;
    HANDLE event = g_event;
    LeaveCriticalSection(&g_task_lock);
    if (event) SetEvent(event);
}
}

void tolk_start_worker() {
    ensure_worker();
}

bool tolk_speak(const wchar_t* text, bool interrupt) {
    if (g_component_dir.empty()) g_component_dir = current_dll_dir();
    return speak_direct(text, interrupt);
}

void tolk_silence() {
    silence_direct();
}

void tolk_queue_speak(const wchar_t* text, bool interrupt) {
    if (!text || !*text) return;
    post_task(task_speak, text, interrupt);
}

void tolk_queue_silence() {
    post_task(task_silence, L"", true);
}

void tolk_shutdown() {
    init_locks_once();
    HANDLE worker = nullptr;
    HANDLE event = nullptr;
    EnterCriticalSection(&g_task_lock);
    if (g_worker) {
        g_worker_quit = true;
        g_task = task_quit;
        worker = g_worker;
        event = g_event;
    }
    LeaveCriticalSection(&g_task_lock);

    if (event) SetEvent(event);
    if (worker) WaitForSingleObject(worker, 5000);

    EnterCriticalSection(&g_task_lock);
    if (g_worker) {
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    if (g_event) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
    g_worker_quit = false;
    g_task = task_none;
    g_task_text.clear();
    LeaveCriticalSection(&g_task_lock);

    unload_direct();
}
