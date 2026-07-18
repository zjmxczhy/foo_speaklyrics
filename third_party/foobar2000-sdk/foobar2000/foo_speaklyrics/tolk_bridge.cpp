#include "stdafx.h"
#include "tolk_bridge.h"
#include "speaklyrics_log.h"

namespace {
using Tolk_Load_t = void(__cdecl*)();
using Tolk_Unload_t = void(__cdecl*)();
using Tolk_TrySAPI_t = void(__cdecl*)(bool);
using Tolk_DetectScreenReader_t = const wchar_t* (__cdecl*)();
using Tolk_Output_t = bool(__cdecl*)(const wchar_t*, bool);
using Tolk_Speak_t = bool(__cdecl*)(const wchar_t*, bool);
using Tolk_Silence_t = bool(__cdecl*)();

enum task_kind { task_none, task_preload, task_speak, task_silence, task_quit };

struct queued_task {
    task_kind kind = task_none;
    tolk_speech_task_type speech_type = tolk_speech_task_type::general;
    std::wstring text;
    bool interrupt = true;
    unsigned retry_attempt = 0;
    ULONGLONG sequence = 0;
    ULONGLONG expires_at = 0;
};

constexpr DWORD kRetryDelaysMs[] = { 200, 500, 1000 };

HMODULE g_tolk = nullptr;
Tolk_Load_t pLoad = nullptr;
Tolk_Unload_t pUnload = nullptr;
Tolk_TrySAPI_t pTrySAPI = nullptr;
Tolk_DetectScreenReader_t pDetectScreenReader = nullptr;
Tolk_Output_t pOutput = nullptr;
Tolk_Speak_t pSpeak = nullptr;
Tolk_Silence_t pSilence = nullptr;
bool g_loaded = false;
CRITICAL_SECTION g_tolk_lock;
CRITICAL_SECTION g_task_lock;

HANDLE g_worker = nullptr;
HANDLE g_event = nullptr;
bool g_worker_quit = false;
queued_task g_task;
ULONGLONG g_task_sequence = 0;
std::wstring g_component_dir;

void init_locks_once() {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&g_tolk_lock);
        InitializeCriticalSection(&g_task_lock);
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

bool safe_detect_screen_reader(Tolk_DetectScreenReader_t fn, std::wstring& name) {
    name.clear();
    if (!fn) return true;
    __try {
        const wchar_t* detected = fn();
        if (detected && *detected) {
            name = detected;
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
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

void clear_tolk_exports() {
    pLoad = nullptr;
    pUnload = nullptr;
    pTrySAPI = nullptr;
    pDetectScreenReader = nullptr;
    pOutput = nullptr;
    pSpeak = nullptr;
    pSilence = nullptr;
}

bool ensure_loaded() {
    if (g_loaded && (pSpeak || pOutput)) return true;

    if (g_component_dir.empty()) g_component_dir = current_dll_dir();

    std::wstring tolk_dir = g_component_dir;
    if (!tolk_dir.empty()) tolk_dir += L"\\tolk";

    // Tolk resolves its screen reader support DLLs by file name.
    if (!tolk_dir.empty()) SetDllDirectoryW(tolk_dir.c_str());

    std::wstring path = tolk_dir;
    if (!path.empty()) path += L"\\Tolk.dll";
    g_tolk = LoadLibraryW(path.empty() ? L"Tolk.dll" : path.c_str());
    if (!g_tolk) g_tolk = LoadLibraryW(L"Tolk.dll");
    if (!g_tolk) {
        speaklyrics_log_error(L"Tolk：无法加载 Tolk.dll，路径：%s，错误码：%lu。", path.c_str(), GetLastError());
        return false;
    }

    pLoad = reinterpret_cast<Tolk_Load_t>(GetProcAddress(g_tolk, "Tolk_Load"));
    pUnload = reinterpret_cast<Tolk_Unload_t>(GetProcAddress(g_tolk, "Tolk_Unload"));
    pTrySAPI = reinterpret_cast<Tolk_TrySAPI_t>(GetProcAddress(g_tolk, "Tolk_TrySAPI"));
    pDetectScreenReader = reinterpret_cast<Tolk_DetectScreenReader_t>(GetProcAddress(g_tolk, "Tolk_DetectScreenReader"));
    pOutput = reinterpret_cast<Tolk_Output_t>(GetProcAddress(g_tolk, "Tolk_Output"));
    pSpeak = reinterpret_cast<Tolk_Speak_t>(GetProcAddress(g_tolk, "Tolk_Speak"));
    pSilence = reinterpret_cast<Tolk_Silence_t>(GetProcAddress(g_tolk, "Tolk_Silence"));
    if (!pLoad || !pUnload || (!pSpeak && !pOutput)) {
        speaklyrics_log_error(L"Tolk：Tolk.dll 缺少必要导出函数。");
        FreeLibrary(g_tolk);
        g_tolk = nullptr;
        clear_tolk_exports();
        return false;
    }
    safe_try_sapi(pTrySAPI, false);
    if (!safe_load(pLoad)) {
        speaklyrics_log_error(L"Tolk：Tolk_Load 调用失败，可能是读屏驱动或 Tolk 依赖异常。");
        // Some drivers are unsafe to unload after a partially completed load.
        clear_tolk_exports();
        return false;
    }
    g_loaded = true;
    speaklyrics_log_info(L"Tolk：加载成功。");
    return true;
}

bool preload_direct(std::wstring& detectedReader) {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    bool ready = ensure_loaded();
    if (ready) ready = safe_detect_screen_reader(pDetectScreenReader, detectedReader);
    LeaveCriticalSection(&g_tolk_lock);
    return ready;
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
    // Do not unload Tolk here. Some screen reader drivers crash during unload;
    // the operating system will reclaim the module when foobar2000 exits.
    if (g_loaded && pSilence) safe_silence(pSilence);
    LeaveCriticalSection(&g_tolk_lock);
}

const wchar_t* speech_task_name(tolk_speech_task_type type) {
    switch (type) {
    case tolk_speech_task_type::lyric: return L"歌词";
    case tolk_speech_task_type::auto_speak_enabled: return L"自动朗读已打开";
    case tolk_speech_task_type::auto_speak_disabled: return L"自动朗读已关闭";
    default: return L"普通提示";
    }
}

const wchar_t* task_name(const queued_task& task) {
    return task.kind == task_preload ? L"预加载" : speech_task_name(task.speech_type);
}

bool task_can_retry(const queued_task& task) {
    if (task.kind == task_preload) return true;
    if (task.kind != task_speak) return false;
    return task.speech_type != tolk_speech_task_type::general;
}

bool task_retry_expired(const queued_task& task) {
    return task.retry_attempt > 0 && task.expires_at != 0 && GetTickCount64() >= task.expires_at;
}

bool task_expiration_reached(const queued_task& task) {
    return task.expires_at != 0 && GetTickCount64() >= task.expires_at;
}

DWORD wait_until(ULONGLONG dueTick) {
    ULONGLONG now = GetTickCount64();
    if (dueTick <= now) return 0;
    ULONGLONG remaining = dueTick - now;
    return remaining >= MAXDWORD ? MAXDWORD - 1 : static_cast<DWORD>(remaining);
}

bool should_log_retry_cancel(const queued_task& task) {
    return task.kind == task_preload ||
        task.speech_type == tolk_speech_task_type::auto_speak_enabled ||
        task.speech_type == tolk_speech_task_type::auto_speak_disabled;
}

DWORD WINAPI worker_proc(void*) {
    queued_task retryTask;
    ULONGLONG retryDueTick = 0;

    for (;;) {
        DWORD timeout = retryTask.kind == task_none ? INFINITE : wait_until(retryDueTick);
        DWORD waitResult = WaitForSingleObject(g_event, timeout);

        queued_task task;
        ULONGLONG currentSequence = 0;
        bool quit = false;

        if (waitResult == WAIT_OBJECT_0) {
            EnterCriticalSection(&g_task_lock);
            currentSequence = g_task_sequence;
            quit = g_worker_quit || g_task.kind == task_quit;
            if (!quit && g_task.kind != task_none) {
                task = std::move(g_task);
                g_task = queued_task{};
            }
            LeaveCriticalSection(&g_task_lock);

            if (quit) break;
            if (retryTask.kind != task_none && retryTask.sequence != currentSequence) {
                if (should_log_retry_cancel(retryTask)) {
                    speaklyrics_log_info(L"Tolk：%s任务重试已被更新的语音任务取消。", task_name(retryTask));
                }
                retryTask = queued_task{};
                retryDueTick = 0;
            }
            if (task.kind == task_none) continue;
        } else if (waitResult == WAIT_TIMEOUT) {
            EnterCriticalSection(&g_task_lock);
            currentSequence = g_task_sequence;
            quit = g_worker_quit;
            LeaveCriticalSection(&g_task_lock);

            if (quit) break;
            if (retryTask.kind == task_none || retryTask.sequence != currentSequence) {
                retryTask = queued_task{};
                retryDueTick = 0;
                continue;
            }
            task = std::move(retryTask);
            retryTask = queued_task{};
            retryDueTick = 0;
        } else {
            speaklyrics_log_error(L"Tolk：语音工作线程等待失败，错误码：%lu。", GetLastError());
            break;
        }

        if (task_retry_expired(task)) {
            speaklyrics_log_info(L"Tolk：%s任务重试已过期并取消。", task_name(task));
            continue;
        }

        bool ok = true;
        std::wstring detectedReader;
        if (task.kind == task_preload) {
            ok = preload_direct(detectedReader);
        } else if (task.kind == task_silence) {
            silence_direct();
        } else if (task.kind == task_speak) {
            ok = speak_direct(task.text.c_str(), task.interrupt);
        }

        if (ok) {
            if (task.kind == task_preload) {
                if (detectedReader.empty()) speaklyrics_log_info(L"Tolk：预加载完成。");
                else speaklyrics_log_info(L"Tolk：预加载完成，已检测到屏幕阅读器：%s。", detectedReader.c_str());
            } else if (task.retry_attempt > 0) {
                speaklyrics_log_info(L"Tolk：%s任务第%u次重试成功。", task_name(task), task.retry_attempt);
            }
            continue;
        }

        if (task_can_retry(task) && task.retry_attempt < static_cast<unsigned>(_countof(kRetryDelaysMs)) && !task_expiration_reached(task)) {
            DWORD delay = kRetryDelaysMs[task.retry_attempt];
            ++task.retry_attempt;
            retryDueTick = GetTickCount64() + delay;
            speaklyrics_log_warning(L"Tolk：%s任务调用失败，将在%lu毫秒后进行第%u次重试。",
                task_name(task), delay, task.retry_attempt);
            retryTask = std::move(task);
        } else {
            speaklyrics_log_warning(L"Tolk：%s任务调用失败，已停止重试。", task_name(task));
        }
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
    g_worker_quit = false;
    g_task = queued_task{};
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

void post_speech_task(const wchar_t* text, bool interrupt, tolk_speech_task_type type, unsigned retryWindowMs) {
    if (!text || !*text || !ensure_worker()) return;

    bool droppedForLyric = false;
    bool lyricReplacedEnabledNotice = false;
    HANDLE event = nullptr;

    EnterCriticalSection(&g_task_lock);
    if (g_worker_quit) {
        LeaveCriticalSection(&g_task_lock);
        return;
    }

    if (type == tolk_speech_task_type::auto_speak_enabled &&
        g_task.kind == task_speak && g_task.speech_type == tolk_speech_task_type::lyric) {
        droppedForLyric = true;
    } else {
        lyricReplacedEnabledNotice = type == tolk_speech_task_type::lyric &&
            g_task.kind == task_speak && g_task.speech_type == tolk_speech_task_type::auto_speak_enabled;

        queued_task task;
        task.kind = task_speak;
        task.speech_type = type;
        task.text = text;
        task.interrupt = interrupt;
        task.sequence = ++g_task_sequence;
        if (retryWindowMs > 0) task.expires_at = GetTickCount64() + retryWindowMs;
        g_task = std::move(task);
        event = g_event;
    }
    LeaveCriticalSection(&g_task_lock);

    if (droppedForLyric) {
        speaklyrics_log_info(L"Tolk：自动朗读已打开提示未覆盖等待中的歌词任务。");
        return;
    }
    if (lyricReplacedEnabledNotice) {
        speaklyrics_log_info(L"Tolk：歌词任务已覆盖等待中的自动朗读已打开提示。");
    }
    if (event) SetEvent(event);
}

void post_simple_task(task_kind kind) {
    if (!ensure_worker()) return;
    HANDLE event = nullptr;
    EnterCriticalSection(&g_task_lock);
    if (!g_worker_quit) {
        queued_task task;
        task.kind = kind;
        task.sequence = ++g_task_sequence;
        g_task = std::move(task);
        event = g_event;
    }
    LeaveCriticalSection(&g_task_lock);
    if (event) SetEvent(event);
}
}

void tolk_preload() {
    if (!ensure_worker()) {
        speaklyrics_log_error(L"Tolk：无法启动预加载工作线程。");
        return;
    }

    HANDLE event = nullptr;
    EnterCriticalSection(&g_task_lock);
    if (!g_worker_quit && g_task.kind == task_none) {
        queued_task task;
        task.kind = task_preload;
        task.sequence = ++g_task_sequence;
        task.expires_at = GetTickCount64() + 2500;
        g_task = std::move(task);
        event = g_event;
    }
    LeaveCriticalSection(&g_task_lock);
    if (event) SetEvent(event);
}

bool tolk_speak(const wchar_t* text, bool interrupt) {
    bool ok = speak_direct(text, interrupt);
    if (!ok) speaklyrics_log_warning(L"Tolk：同步朗读调用失败。");
    return ok;
}

void tolk_silence() {
    silence_direct();
}

void tolk_queue_speak(const wchar_t* text, bool interrupt) {
    post_speech_task(text, interrupt, tolk_speech_task_type::general, 0);
}

void tolk_queue_speak_task(const wchar_t* text, bool interrupt, tolk_speech_task_type type, unsigned retryWindowMs) {
    post_speech_task(text, interrupt, type, retryWindowMs);
}

void tolk_queue_silence() {
    post_simple_task(task_silence);
}

void tolk_invalidate_pending() {
    init_locks_once();
    HANDLE event = nullptr;
    EnterCriticalSection(&g_task_lock);
    ++g_task_sequence;
    if (g_task.kind == task_speak) g_task = queued_task{};
    event = g_event;
    LeaveCriticalSection(&g_task_lock);
    if (event) SetEvent(event);
}

void tolk_shutdown() {
    init_locks_once();
    HANDLE worker = nullptr;
    HANDLE event = nullptr;
    EnterCriticalSection(&g_task_lock);
    if (g_worker) {
        g_worker_quit = true;
        queued_task task;
        task.kind = task_quit;
        task.sequence = ++g_task_sequence;
        g_task = std::move(task);
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
    g_task = queued_task{};
    LeaveCriticalSection(&g_task_lock);

    unload_direct();
}
