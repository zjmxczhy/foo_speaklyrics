#include "stdafx.h"
#include "tolk_bridge.h"
#include "speech_engine.h"
#include "speaklyrics_log.h"
#include "speech_task_queue.h"
#include "tolk_call_policy.h"

#include <memory>

namespace {
using Tolk_Load_t = void(__cdecl*)();
using Tolk_Unload_t = void(__cdecl*)();
using Tolk_TrySAPI_t = void(__cdecl*)(bool);
using Tolk_DetectScreenReader_t = const wchar_t* (__cdecl*)();
using Tolk_Speak_t = bool(__cdecl*)(const wchar_t*, bool);
using Tolk_Silence_t = bool(__cdecl*)();

enum class worker_lifecycle_state {
    stopped,
    running,
    stopping,
};

struct worker_context {
    HANDLE event = nullptr;
    HANDLE stopped_event = nullptr;

    worker_context() = default;
    worker_context(const worker_context&) = delete;
    worker_context& operator=(const worker_context&) = delete;

    ~worker_context() {
        if (event) CloseHandle(event);
        if (stopped_event) CloseHandle(stopped_event);
    }
};

using worker_context_ptr = std::shared_ptr<worker_context>;

constexpr DWORD kRetryDelaysMs[] = { 200, 500, 1000 };

HMODULE g_tolk = nullptr;
Tolk_Load_t pLoad = nullptr;
Tolk_Unload_t pUnload = nullptr;
Tolk_TrySAPI_t pTrySAPI = nullptr;
Tolk_DetectScreenReader_t pDetectScreenReader = nullptr;
Tolk_Speak_t pSpeak = nullptr;
Tolk_Silence_t pSilence = nullptr;
bool g_loaded = false;
CRITICAL_SECTION g_tolk_lock;
CRITICAL_SECTION g_task_lock;

worker_context_ptr g_worker_context;
worker_lifecycle_state g_worker_state = worker_lifecycle_state::stopped;
bool g_shutdown_requested = false;
speech_task_queue g_tasks;
uint64_t g_active_generation = 1;
speech_invalidation_reason g_last_invalidation_reason = speech_invalidation_reason::none;
bool g_silence_requested = false;
bool g_has_active_task = false;
speech_task g_active_task;
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
tolk_text_call_result safe_text_call(Fn fn, const wchar_t* text, bool interrupt) {
    if (!fn) return {};
    __try {
        return invoke_tolk_text_once(fn, text, interrupt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return tolk_text_call_threw(static_cast<uint32_t>(GetExceptionCode()));
    }
}

bool safe_silence(Tolk_Silence_t fn) {
    if (!fn) return true;
    __try { return fn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring current_dll_dir() {
    pfc::string8 path = core_api::get_my_full_path();
    std::wstring wide = pfc::stringcvt::string_wide_from_utf8(path).get_ptr();
    size_t slash = wide.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : wide.substr(0, slash);
}

class scoped_tolk_load_environment {
public:
    explicit scoped_tolk_load_environment(const std::wstring& directory) {
        m_mutex = CreateMutexW(nullptr, FALSE, L"Local\\foobar2000.tolk-runtime-load");
        if (!m_mutex) return;

        DWORD waitResult = WaitForSingleObject(m_mutex, 10000);
        if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) return;
        m_locked = true;

        DWORD required = GetDllDirectoryW(0, nullptr);
        if (required > 0) {
            std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1);
            DWORD copied = GetDllDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
            if (copied > 0 && copied < buffer.size()) {
                m_previous.assign(buffer.data(), copied);
                m_had_previous = true;
            }
        }

        m_ready = !directory.empty() && SetDllDirectoryW(directory.c_str()) != FALSE;
    }

    ~scoped_tolk_load_environment() {
        if (m_ready) SetDllDirectoryW(m_had_previous ? m_previous.c_str() : nullptr);
        if (m_locked) ReleaseMutex(m_mutex);
        if (m_mutex) CloseHandle(m_mutex);
    }

    bool ready() const { return m_ready; }

private:
    HANDLE m_mutex = nullptr;
    bool m_locked = false;
    bool m_ready = false;
    bool m_had_previous = false;
    std::wstring m_previous;
};

void clear_tolk_exports() {
    pLoad = nullptr;
    pUnload = nullptr;
    pTrySAPI = nullptr;
    pDetectScreenReader = nullptr;
    pSpeak = nullptr;
    pSilence = nullptr;
}

bool ensure_loaded() {
    if (g_loaded && pSpeak) return true;

    if (g_component_dir.empty()) g_component_dir = current_dll_dir();

    std::wstring tolk_dir = g_component_dir;
    if (!tolk_dir.empty()) tolk_dir += L"\\tolk";

    // Tolk resolves its screen reader support DLLs by file name. Serialize the
    // process-wide search-path change with other foobar2000 Tolk components.
    scoped_tolk_load_environment loadEnvironment(tolk_dir);
    if (!loadEnvironment.ready()) {
        speaklyrics_log_error(L"Tolk：无法设置独立运行库目录：%s。", speaklyrics_log_path(tolk_dir.c_str()).c_str());
        return false;
    }

    std::wstring path = tolk_dir;
    if (!path.empty()) path += L"\\Tolk.dll";
    g_tolk = LoadLibraryW(path.c_str());
    if (!g_tolk) {
        speaklyrics_log_error(L"Tolk：无法加载 Tolk.dll，路径：%s，错误码：%lu。", speaklyrics_log_path(path.c_str()).c_str(), GetLastError());
        return false;
    }

    pLoad = reinterpret_cast<Tolk_Load_t>(GetProcAddress(g_tolk, "Tolk_Load"));
    pUnload = reinterpret_cast<Tolk_Unload_t>(GetProcAddress(g_tolk, "Tolk_Unload"));
    pTrySAPI = reinterpret_cast<Tolk_TrySAPI_t>(GetProcAddress(g_tolk, "Tolk_TrySAPI"));
    pDetectScreenReader = reinterpret_cast<Tolk_DetectScreenReader_t>(GetProcAddress(g_tolk, "Tolk_DetectScreenReader"));
    pSpeak = reinterpret_cast<Tolk_Speak_t>(GetProcAddress(g_tolk, "Tolk_Speak"));
    pSilence = reinterpret_cast<Tolk_Silence_t>(GetProcAddress(g_tolk, "Tolk_Silence"));
    if (!pLoad || !pUnload || !pSpeak) {
        speaklyrics_log_error(L"Tolk：Tolk.dll 缺少必要导出函数 Tolk_Load、Tolk_Unload 或 Tolk_Speak。");
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

tolk_text_call_result speak_direct(const wchar_t* text, bool interrupt,
    uint64_t diagnosticTaskId = 0) {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    tolk_text_call_result result = tolk_text_call_returned(true);
    if (text && *text) {
        if (!ensure_loaded()) {
            result = {};
        } else {
            result = safe_text_call(pSpeak, text, interrupt);
        }

        const unsigned long long taskId = static_cast<unsigned long long>(diagnosticTaskId);
        switch (result.status) {
        case tolk_text_call_status::returned_true:
            if (diagnosticTaskId != 0) {
                speaklyrics_log_info(L"Tolk：语音任务=%llu 调用一次 Tolk_Speak，正常返回 true。", taskId);
            }
            break;
        case tolk_text_call_status::returned_false:
            if (diagnosticTaskId != 0) {
                speaklyrics_log_warning(
                    L"Tolk：语音任务=%llu 调用一次 Tolk_Speak，正常返回 false；按照 Tolk 接口约定按已提交处理，不调用 Tolk_Output，也不自动重试。",
                    taskId);
            } else {
                speaklyrics_log_warning(
                    L"Tolk：同步朗读调用一次 Tolk_Speak，正常返回 false；按照 Tolk 接口约定按已提交处理，不调用 Tolk_Output，也不自动重试。");
            }
            break;
        case tolk_text_call_status::exception:
            if (diagnosticTaskId != 0) {
                speaklyrics_log_error(
                    L"Tolk：语音任务=%llu 调用 Tolk_Speak 时发生异常，异常码=0x%08lX；未调用 Tolk_Output。",
                    taskId, static_cast<unsigned long>(result.exception_code));
            } else {
                speaklyrics_log_error(
                    L"Tolk：同步调用 Tolk_Speak 时发生异常，异常码=0x%08lX；未调用 Tolk_Output。",
                    static_cast<unsigned long>(result.exception_code));
            }
            break;
        case tolk_text_call_status::not_called:
        default:
            if (diagnosticTaskId != 0) {
                speaklyrics_log_error(L"Tolk：语音任务=%llu 未能调用 Tolk_Speak。", taskId);
            }
            break;
        }
    }
    LeaveCriticalSection(&g_tolk_lock);
    return result;
}

bool silence_direct() {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    const bool ok = !g_loaded || !pSilence || safe_silence(pSilence);
    LeaveCriticalSection(&g_tolk_lock);
    return ok;
}

void unload_direct() {
    init_locks_once();
    EnterCriticalSection(&g_tolk_lock);
    // Do not unload Tolk here. Some screen reader drivers crash during unload;
    // the operating system will reclaim the module when foobar2000 exits.
    if (g_loaded && pSilence) safe_silence(pSilence);
    LeaveCriticalSection(&g_tolk_lock);
}

const wchar_t* speech_task_name(speech_task_type type) {
    switch (type) {
    case speech_task_type::preload: return L"预加载";
    case speech_task_type::track_announcement: return L"歌曲信息";
    case speech_task_type::lyric: return L"歌词";
    case speech_task_type::auto_speak_enabled: return L"自动朗读已打开";
    case speech_task_type::auto_speak_disabled: return L"自动朗读已关闭";
    case speech_task_type::general:
    default: return L"普通提示";
    }
}

const wchar_t* enqueue_status_name(speech_enqueue_status status) {
    switch (status) {
    case speech_enqueue_status::accepted: return L"已入队";
    case speech_enqueue_status::invalid_input: return L"输入无效";
    case speech_enqueue_status::expired: return L"已经过期";
    case speech_enqueue_status::stale_generation: return L"播放位置世代已经失效";
    case speech_enqueue_status::worker_unavailable: return L"工作线程不可用";
    case speech_enqueue_status::shutting_down: return L"正在退出";
    case speech_enqueue_status::queue_full: return L"队列已满";
    case speech_enqueue_status::signal_failed: return L"工作线程通知失败";
    default: return L"未知";
    }
}

DWORD wait_until(ULONGLONG dueTick) {
    ULONGLONG now = GetTickCount64();
    if (dueTick <= now) return 0;
    ULONGLONG remaining = dueTick - now;
    return remaining >= MAXDWORD ? MAXDWORD - 1 : static_cast<DWORD>(remaining);
}

void report_task_result(speech_task task, speech_task_result_status status,
    speech_invalidation_reason reason = speech_invalidation_reason::none,
    int64_t errorCode = 0) {
    speech_task_result result;
    result.backend = speech_backend_type::tolk;
    result.status = status;
    result.reason = reason;
    result.task = std::move(task);
    result.error_code = errorCode;
    result.completed_at = GetTickCount64();
    speech_report_task_result(std::move(result));
}

void report_expired_tasks(std::vector<speech_task>& tasks) {
    for (auto& task : tasks) {
        report_task_result(std::move(task), speech_task_result_status::expired);
    }
}

void report_canceled_tasks(std::vector<speech_task_cancellation>& tasks) {
    for (auto& canceled : tasks) {
        report_task_result(std::move(canceled.task), speech_task_result_status::canceled,
            canceled.reason);
    }
}

bool clear_active_task_locked(uint64_t taskId) {
    if (!g_has_active_task || g_active_task.task_id != taskId) return false;
    g_active_task = speech_task{};
    g_has_active_task = false;
    return true;
}

bool release_active_task(uint64_t taskId) {
    EnterCriticalSection(&g_task_lock);
    const bool released = clear_active_task_locked(taskId);
    LeaveCriticalSection(&g_task_lock);
    return released;
}

void finish_active_task(speech_task task, speech_task_result_status status,
    speech_invalidation_reason reason = speech_invalidation_reason::none,
    int64_t errorCode = 0) {
    if (!release_active_task(task.task_id)) {
        speaklyrics_log_warning(
            L"Tolk：任务=%llu 的最终结果被忽略，因为该任务已不再由工作线程持有。",
            static_cast<unsigned long long>(task.task_id));
        return;
    }
    report_task_result(std::move(task), status, reason, errorCode);
}

void mark_worker_finished(const worker_context_ptr& context) {
    bool was_current_worker = false;
    bool hadActiveTask = false;
    speech_task activeTask;
    std::vector<speech_task_cancellation> canceled;
    EnterCriticalSection(&g_task_lock);
    if (g_worker_context.get() == context.get()) {
        if (g_has_active_task) {
            activeTask = std::move(g_active_task);
            g_active_task = speech_task{};
            g_has_active_task = false;
            hadActiveTask = true;
        }
        g_tasks.cancel_all(speech_invalidation_reason::worker_exit, canceled);
        g_worker_context.reset();
        g_worker_state = worker_lifecycle_state::stopped;
        g_silence_requested = false;
        was_current_worker = true;
    }
    LeaveCriticalSection(&g_task_lock);

    if (hadActiveTask) {
        report_task_result(std::move(activeTask), speech_task_result_status::failed,
            speech_invalidation_reason::worker_exit, E_FAIL);
    }
    report_canceled_tasks(canceled);

    if (was_current_worker) {
        speaklyrics_log_info(L"Tolk：语音工作线程已退出，工作线程上下文将负责回收事件句柄。");
    }
}

DWORD WINAPI worker_proc(worker_context_ptr context) {
    const HANDLE workerEvent = context->event;

    for (;;) {
        speech_task task;
        std::vector<speech_task> expired;
        bool hasTask = false;
        bool shouldSilence = false;
        bool stopping = false;
        uint64_t nextWakeAt = 0;
        uint64_t activeGeneration = 0;
        speech_invalidation_reason activeReason = speech_invalidation_reason::none;

        const uint64_t now = GetTickCount64();
        EnterCriticalSection(&g_task_lock);
        stopping = g_worker_state == worker_lifecycle_state::stopping;
        if (!stopping) {
            shouldSilence = g_silence_requested;
            g_silence_requested = false;
            hasTask = g_tasks.take_ready(now, task, expired);
            activeGeneration = g_active_generation;
            activeReason = g_last_invalidation_reason;
            if (hasTask) {
                g_active_task = task;
                g_has_active_task = true;
            }
            nextWakeAt = g_tasks.next_wake_at();
        }
        LeaveCriticalSection(&g_task_lock);

        report_expired_tasks(expired);
        if (stopping) break;
        if (shouldSilence && !silence_direct()) {
            speaklyrics_log_warning(L"Tolk：执行失效任务的静音请求失败。");
        }

        if (!hasTask) {
            const DWORD timeout = nextWakeAt == 0 ? INFINITE : wait_until(nextWakeAt);
            const DWORD waitResult = WaitForSingleObject(workerEvent, timeout);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) continue;
            speaklyrics_log_error(L"Tolk：语音工作线程等待失败，错误码：%lu。", GetLastError());
            break;
        }

        EnterCriticalSection(&g_task_lock);
        activeGeneration = g_active_generation;
        activeReason = g_last_invalidation_reason;
        LeaveCriticalSection(&g_task_lock);
        if (task.playback_generation < activeGeneration) {
            finish_active_task(std::move(task), speech_task_result_status::canceled, activeReason);
            continue;
        }
        if (speech_task_is_expired(task, GetTickCount64())) {
            finish_active_task(std::move(task), speech_task_result_status::expired);
            continue;
        }

        bool ok = true;
        bool retryable = true;
        int64_t callError = 0;
        std::wstring detectedReader;
        if (task.type == speech_task_type::preload) {
            ok = preload_direct(detectedReader);
        } else {
            const tolk_text_call_result callResult =
                speak_direct(task.text.c_str(), task.interrupt, task.task_id);
            ok = tolk_text_call_was_dispatched(callResult);
            retryable = tolk_text_call_should_retry(callResult);
            callError = static_cast<int64_t>(callResult.exception_code);
        }

        EnterCriticalSection(&g_task_lock);
        activeGeneration = g_active_generation;
        activeReason = g_last_invalidation_reason;
        LeaveCriticalSection(&g_task_lock);

        if (task.playback_generation < activeGeneration) {
            finish_active_task(std::move(task), speech_task_result_status::canceled, activeReason);
            continue;
        }

        if (ok) {
            if (task.type == speech_task_type::preload) {
                if (detectedReader.empty()) speaklyrics_log_info(L"Tolk：预加载完成。");
                else speaklyrics_log_info(L"Tolk：预加载完成，已检测到屏幕阅读器：%s。", detectedReader.c_str());
            } else if (task.retry_attempt > 0) {
                speaklyrics_log_info(L"Tolk：%s任务=%llu 第%u次重试成功。", speech_task_name(task.type),
                    static_cast<unsigned long long>(task.task_id), task.retry_attempt);
            }
            finish_active_task(std::move(task), speech_task_result_status::dispatched);
            continue;
        }

        if (speech_task_is_expired(task, GetTickCount64())) {
            finish_active_task(std::move(task), speech_task_result_status::expired);
            continue;
        }

        if (retryable && speech_task_can_retry(task) &&
            task.retry_attempt < static_cast<unsigned>(_countof(kRetryDelaysMs)) &&
            !speech_task_is_expired(task, GetTickCount64())) {
            const DWORD delay = kRetryDelaysMs[task.retry_attempt];
            ++task.retry_attempt;
            task.ready_at = GetTickCount64() + delay;
            speaklyrics_log_warning(L"Tolk：%s任务=%llu 调用失败，将在%lu毫秒后进行第%u次重试。",
                speech_task_name(task.type), static_cast<unsigned long long>(task.task_id),
                delay, task.retry_attempt);

            std::vector<speech_task> retryExpired;
            std::vector<speech_task_cancellation> retryCanceled;
            speech_enqueue_status retryStatus = speech_enqueue_status::shutting_down;
            EnterCriticalSection(&g_task_lock);
            if (!g_shutdown_requested && g_worker_state == worker_lifecycle_state::running) {
                if (task.playback_generation < g_active_generation) {
                    retryStatus = speech_enqueue_status::stale_generation;
                    activeReason = g_last_invalidation_reason;
                } else {
                    retryStatus = g_tasks.requeue_retry(task, GetTickCount64(),
                        retryExpired, retryCanceled);
                    if (retryStatus == speech_enqueue_status::accepted) {
                        clear_active_task_locked(task.task_id);
                    }
                }
            }
            LeaveCriticalSection(&g_task_lock);
            report_expired_tasks(retryExpired);
            report_canceled_tasks(retryCanceled);
            if (retryStatus == speech_enqueue_status::accepted) continue;
            speaklyrics_log_warning(
                L"Tolk：%s任务=%llu 第%u次重试重新入队失败，原因=%s。",
                speech_task_name(task.type), static_cast<unsigned long long>(task.task_id),
                task.retry_attempt, enqueue_status_name(retryStatus));
            if (retryStatus == speech_enqueue_status::stale_generation) {
                finish_active_task(std::move(task), speech_task_result_status::canceled, activeReason);
                continue;
            }
            if (retryStatus == speech_enqueue_status::expired) {
                finish_active_task(std::move(task), speech_task_result_status::expired);
                continue;
            }
            if (retryStatus == speech_enqueue_status::shutting_down) {
                finish_active_task(std::move(task), speech_task_result_status::canceled,
                    speech_invalidation_reason::shutdown);
                continue;
            }
            const speech_invalidation_reason failureReason =
                retryStatus == speech_enqueue_status::queue_full ?
                speech_invalidation_reason::queue_pressure : speech_invalidation_reason::worker_exit;
            finish_active_task(std::move(task), speech_task_result_status::failed,
                failureReason, static_cast<int64_t>(retryStatus));
            continue;
        }

        speaklyrics_log_warning(L"Tolk：%s任务=%llu 调用失败，已停止重试。",
            speech_task_name(task.type), static_cast<unsigned long long>(task.task_id));
        finish_active_task(std::move(task), speech_task_result_status::failed,
            speech_invalidation_reason::none, callError);
    }

    if (!silence_direct()) speaklyrics_log_warning(L"Tolk：工作线程退出时静音失败。");
    unload_direct();
    return 0;
}

bool ensure_worker() {
    init_locks_once();
    EnterCriticalSection(&g_task_lock);
    if (g_shutdown_requested || g_worker_state == worker_lifecycle_state::stopping) {
        LeaveCriticalSection(&g_task_lock);
        return false;
    }
    if (g_worker_state == worker_lifecycle_state::running) {
        const bool ready = g_worker_context &&
            g_worker_context->event != nullptr && g_worker_context->stopped_event != nullptr;
        LeaveCriticalSection(&g_task_lock);
        return ready;
    }

    worker_context_ptr context;
    try {
        context = std::make_shared<worker_context>();
    } catch (...) {
        LeaveCriticalSection(&g_task_lock);
        return false;
    }

    context->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!context->event) {
        LeaveCriticalSection(&g_task_lock);
        return false;
    }
    context->stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!context->stopped_event) {
        LeaveCriticalSection(&g_task_lock);
        return false;
    }

    g_worker_context = context;
    g_worker_state = worker_lifecycle_state::running;
    try {
        // splitTask keeps the component loaded until the worker has returned.
        // The shared context keeps both event handles alive until every worker
        // and shutdown-side reference has been released.
        fb2k::splitTask([context]() {
            try {
                worker_proc(context);
            } catch (...) {
                speaklyrics_log_error(L"Tolk：语音工作线程发生未知异常。");
            }
            mark_worker_finished(context);
            if (!SetEvent(context->stopped_event)) {
                speaklyrics_log_error(L"Tolk：无法标记语音工作线程结束，错误码：%lu。", GetLastError());
            }
        });
    } catch (...) {
        g_worker_context.reset();
        g_worker_state = worker_lifecycle_state::stopped;
        LeaveCriticalSection(&g_task_lock);
        return false;
    }
    LeaveCriticalSection(&g_task_lock);
    return true;
}

}

bool tolk_speak(const wchar_t* text, bool interrupt) {
    const tolk_text_call_result result = speak_direct(text, interrupt);
    const bool ok = tolk_text_call_was_dispatched(result);
    if (!ok) speaklyrics_log_warning(L"Tolk：同步朗读调用失败。");
    return ok;
}

void tolk_silence() {
    silence_direct();
}

speech_enqueue_result tolk_queue_task(speech_task task) {
    init_locks_once();
    if (task.task_id == 0) task.task_id = speech_allocate_task_id();
    speech_enqueue_result result;
    result.task_id = task.task_id;
    result.playback_generation = task.playback_generation;
    result.expires_at = task.expires_at;

    if (task.type != speech_task_type::preload && task.text.empty()) {
        result.status = speech_enqueue_status::invalid_input;
        return result;
    }
    if (speech_task_is_expired(task, GetTickCount64())) {
        result.status = speech_enqueue_status::expired;
        return result;
    }
    if (!ensure_worker()) {
        EnterCriticalSection(&g_task_lock);
        result.status = g_shutdown_requested ? speech_enqueue_status::shutting_down :
            speech_enqueue_status::worker_unavailable;
        LeaveCriticalSection(&g_task_lock);
        return result;
    }

    std::vector<speech_task> expired;
    std::vector<speech_task_cancellation> canceled;
    DWORD signalError = ERROR_SUCCESS;
    EnterCriticalSection(&g_task_lock);
    if (g_shutdown_requested || g_worker_state == worker_lifecycle_state::stopping) {
        result.status = speech_enqueue_status::shutting_down;
    } else if (g_worker_state != worker_lifecycle_state::running || !g_worker_context) {
        result.status = speech_enqueue_status::worker_unavailable;
    } else if (task.playback_generation < g_active_generation) {
        result.status = speech_enqueue_status::stale_generation;
    } else {
        result.status = g_tasks.enqueue(std::move(task), GetTickCount64(), expired, canceled);
        result.queue_depth = g_tasks.size();
        if (result.accepted()) {
            if (!g_worker_context || !g_worker_context->event) {
                g_tasks.remove(result.task_id);
                result.status = speech_enqueue_status::worker_unavailable;
                result.queue_depth = g_tasks.size();
            } else if (!SetEvent(g_worker_context->event)) {
                signalError = GetLastError();
                g_tasks.remove(result.task_id);
                result.status = speech_enqueue_status::signal_failed;
                result.queue_depth = g_tasks.size();
            }
        }
    }
    LeaveCriticalSection(&g_task_lock);
    report_expired_tasks(expired);
    report_canceled_tasks(canceled);
    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"Tolk：任务=%llu 已撤回，无法通知工作线程，错误码：%lu。",
            static_cast<unsigned long long>(result.task_id), signalError);
    }
    return result;
}

void tolk_invalidate_pending(uint64_t activeGeneration, speech_invalidation_reason reason) {
    init_locks_once();
    std::vector<speech_task_cancellation> canceled;
    DWORD signalError = ERROR_SUCCESS;
    uint64_t activeTaskId = 0;
    bool workerAvailable = false;
    EnterCriticalSection(&g_task_lock);
    if (activeGeneration > g_active_generation) g_active_generation = activeGeneration;
    g_last_invalidation_reason = reason;
    g_tasks.invalidate_before_generation(g_active_generation, reason, canceled);
    if (g_has_active_task && g_active_task.playback_generation < g_active_generation) {
        activeTaskId = g_active_task.task_id;
    }
    workerAvailable = g_worker_state == worker_lifecycle_state::running &&
        g_worker_context != nullptr;
    if (workerAvailable) {
        g_silence_requested = true;
        if (!SetEvent(g_worker_context->event)) signalError = GetLastError();
    }
    LeaveCriticalSection(&g_task_lock);

    report_canceled_tasks(canceled);
    if (workerAvailable || !canceled.empty() || activeTaskId != 0) {
        speaklyrics_log_info(
            L"Tolk：任务失效完成，播放世代=%llu，已取消等待任务=%llu，正在调用的旧任务=%llu。",
            static_cast<unsigned long long>(activeGeneration),
            static_cast<unsigned long long>(canceled.size()),
            static_cast<unsigned long long>(activeTaskId));
    }
    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"Tolk：无法通知工作线程执行失效和静音，错误码：%lu。", signalError);
    }
}

void tolk_shutdown() {
    init_locks_once();
    worker_context_ptr context;
    std::vector<speech_task_cancellation> canceled;
    bool should_signal = false;
    DWORD signalError = ERROR_SUCCESS;
    EnterCriticalSection(&g_task_lock);
    g_shutdown_requested = true;
    g_tasks.cancel_all(speech_invalidation_reason::shutdown, canceled);
    g_silence_requested = true;
    if (g_worker_state == worker_lifecycle_state::running && g_worker_context) {
        g_worker_state = worker_lifecycle_state::stopping;
        context = g_worker_context;
        should_signal = true;
    } else if (g_worker_state == worker_lifecycle_state::stopping) {
        // A previous shutdown may still be waiting for a blocked Tolk call.
        // Reuse the same context and wait again instead of abandoning cleanup.
        context = g_worker_context;
        should_signal = context != nullptr;
    }
    if (should_signal && context && !SetEvent(context->event)) {
        signalError = GetLastError();
    }
    LeaveCriticalSection(&g_task_lock);
    report_canceled_tasks(canceled);

    if (!context) {
        unload_direct();
        return;
    }

    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"Tolk：无法通知语音工作线程退出，错误码：%lu。", signalError);
    }

    DWORD waitError = ERROR_SUCCESS;
    const DWORD waitResult = context->stopped_event
        ? WaitForSingleObject(context->stopped_event, 5000) : WAIT_FAILED;
    if (waitResult == WAIT_FAILED) {
        waitError = context->stopped_event ? GetLastError() : ERROR_INVALID_HANDLE;
    }
    if (waitResult != WAIT_OBJECT_0) {
        if (waitResult == WAIT_TIMEOUT) {
            speaklyrics_log_error(
                L"Tolk：语音工作线程在5秒内未退出；保留工作线程上下文并禁止重新启动，线程结束后将自动回收事件句柄。");
        } else {
            speaklyrics_log_error(
                L"Tolk：等待语音工作线程退出失败，错误码：%lu；保留工作线程上下文并禁止重新启动。",
                waitError);
        }
        return;
    }

    EnterCriticalSection(&g_task_lock);
    if (g_worker_context.get() == context.get()) {
        // mark_worker_finished() normally clears this before stopped_event is
        // signaled. Keep this as an idempotent fallback for an abnormal path.
        g_worker_context.reset();
        g_worker_state = worker_lifecycle_state::stopped;
        g_silence_requested = false;
    }
    LeaveCriticalSection(&g_task_lock);

    speaklyrics_log_info(L"Tolk：语音工作线程已安全停止。");
}
