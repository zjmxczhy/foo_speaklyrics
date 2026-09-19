#include "stdafx.h"
#include "sapi_speech.h"
#include "speech_engine.h"
#include "speaklyrics_log.h"
#include "speech_task_queue.h"

#include <sapi.h>
#include <memory>

namespace {

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

CRITICAL_SECTION g_sapi_lock;
bool g_sapi_lock_ready = false;
worker_context_ptr g_sapi_worker_context;
worker_lifecycle_state g_sapi_worker_state = worker_lifecycle_state::stopped;
bool g_sapi_shutdown_requested = false;
speech_task_queue g_sapi_tasks;
uint64_t g_sapi_active_generation = 1;
speech_invalidation_reason g_sapi_last_invalidation_reason = speech_invalidation_reason::none;
bool g_sapi_purge_requested = false;
bool g_sapi_has_active_task = false;
speech_task g_sapi_active_task;

constexpr DWORD kRetryDelaysMs[] = { 200, 500, 1000 };

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

HRESULT purge_sapi_queue_unsafe(ISpVoice* voice) {
    if (!voice) return E_POINTER;
    return voice->Speak(nullptr, SPF_PURGEBEFORESPEAK | SPF_ASYNC, nullptr);
}

bool speak_with_voice(ISpVoice* voice, const wchar_t* voiceType, const wchar_t* voiceId, int rate, const wchar_t* text, bool interrupt) {
    __try {
        return speak_with_voice_unsafe(voice, voiceType, voiceId, rate, text, interrupt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

HRESULT purge_sapi_queue(ISpVoice* voice) {
    __try {
        return purge_sapi_queue_unsafe(voice);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
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
    const ULONGLONG now = GetTickCount64();
    if (dueTick <= now) return 0;
    const ULONGLONG remaining = dueTick - now;
    return remaining >= MAXDWORD ? MAXDWORD - 1 : static_cast<DWORD>(remaining);
}

void report_task_result(speech_task task, speech_task_result_status status,
    speech_invalidation_reason reason = speech_invalidation_reason::none,
    int64_t errorCode = 0) {
    speech_task_result result;
    result.backend = speech_backend_type::sapi;
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
    if (!g_sapi_has_active_task || g_sapi_active_task.task_id != taskId) return false;
    g_sapi_active_task = speech_task{};
    g_sapi_has_active_task = false;
    return true;
}

bool release_active_task(uint64_t taskId) {
    EnterCriticalSection(&g_sapi_lock);
    const bool released = clear_active_task_locked(taskId);
    LeaveCriticalSection(&g_sapi_lock);
    return released;
}

void finish_active_task(speech_task task, speech_task_result_status status,
    speech_invalidation_reason reason = speech_invalidation_reason::none,
    int64_t errorCode = 0) {
    if (!release_active_task(task.task_id)) {
        speaklyrics_log_warning(
            L"SAPI：任务=%llu 的最终结果被忽略，因为该任务已不再由工作线程持有。",
            static_cast<unsigned long long>(task.task_id));
        return;
    }
    report_task_result(std::move(task), status, reason, errorCode);
}

bool ensure_voice(ISpVoice*& voice) {
    if (voice) return true;
    const HRESULT result = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL,
        IID_ISpVoice, reinterpret_cast<void**>(&voice));
    if (FAILED(result)) voice = nullptr;
    return voice != nullptr;
}

void mark_worker_finished(const worker_context_ptr& context) {
    bool was_current_worker = false;
    bool hadActiveTask = false;
    speech_task activeTask;
    std::vector<speech_task_cancellation> canceled;
    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_worker_context.get() == context.get()) {
        if (g_sapi_has_active_task) {
            activeTask = std::move(g_sapi_active_task);
            g_sapi_active_task = speech_task{};
            g_sapi_has_active_task = false;
            hadActiveTask = true;
        }
        g_sapi_tasks.cancel_all(speech_invalidation_reason::worker_exit, canceled);
        g_sapi_worker_context.reset();
        g_sapi_worker_state = worker_lifecycle_state::stopped;
        g_sapi_purge_requested = false;
        was_current_worker = true;
    }
    LeaveCriticalSection(&g_sapi_lock);

    if (hadActiveTask) {
        report_task_result(std::move(activeTask), speech_task_result_status::failed,
            speech_invalidation_reason::worker_exit, E_FAIL);
    }
    report_canceled_tasks(canceled);

    if (was_current_worker) {
        speaklyrics_log_info(L"SAPI：语音工作线程已退出，工作线程上下文将负责回收事件句柄。");
    }
}

DWORD WINAPI sapi_worker_proc(worker_context_ptr context) {
    const HANDLE workerEvent = context->event;
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool coInitialized = SUCCEEDED(hrCo);
    ISpVoice* voice = nullptr;
    ensure_voice(voice);

    for (;;) {
        speech_task task;
        std::vector<speech_task> expired;
        bool hasTask = false;
        bool shouldPurge = false;
        bool stopping = false;
        uint64_t nextWakeAt = 0;
        uint64_t activeGeneration = 0;
        speech_invalidation_reason activeReason = speech_invalidation_reason::none;

        const uint64_t now = GetTickCount64();
        EnterCriticalSection(&g_sapi_lock);
        stopping = g_sapi_worker_state == worker_lifecycle_state::stopping;
        if (!stopping) {
            shouldPurge = g_sapi_purge_requested;
            g_sapi_purge_requested = false;
            hasTask = g_sapi_tasks.take_ready(now, task, expired);
            activeGeneration = g_sapi_active_generation;
            activeReason = g_sapi_last_invalidation_reason;
            if (hasTask) {
                g_sapi_active_task = task;
                g_sapi_has_active_task = true;
            }
            nextWakeAt = g_sapi_tasks.next_wake_at();
        }
        LeaveCriticalSection(&g_sapi_lock);

        report_expired_tasks(expired);
        if (stopping) break;
        if (shouldPurge) {
            const HRESULT purgeResult = purge_sapi_queue(voice);
            if (FAILED(purgeResult)) {
                speaklyrics_log_warning(L"SAPI：执行失效任务的清空请求失败，HRESULT=0x%08lX。",
                    static_cast<unsigned long>(purgeResult));
            }
        }

        if (!hasTask) {
            const DWORD timeout = nextWakeAt == 0 ? INFINITE : wait_until(nextWakeAt);
            const DWORD waitResult = WaitForSingleObject(workerEvent, timeout);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) continue;
            speaklyrics_log_error(L"SAPI：语音工作线程等待失败，错误码：%lu。", GetLastError());
            break;
        }

        EnterCriticalSection(&g_sapi_lock);
        activeGeneration = g_sapi_active_generation;
        activeReason = g_sapi_last_invalidation_reason;
        LeaveCriticalSection(&g_sapi_lock);
        if (task.playback_generation < activeGeneration) {
            finish_active_task(std::move(task), speech_task_result_status::canceled, activeReason);
            continue;
        }
        if (speech_task_is_expired(task, GetTickCount64())) {
            finish_active_task(std::move(task), speech_task_result_status::expired);
            continue;
        }

        bool ok = ensure_voice(voice);
        if (ok && task.type != speech_task_type::preload) {
            ok = speak_with_voice(voice, task.voice_type.c_str(), task.voice_id.c_str(),
                task.voice_rate, task.text.c_str(), task.interrupt);
        }

        EnterCriticalSection(&g_sapi_lock);
        activeGeneration = g_sapi_active_generation;
        activeReason = g_sapi_last_invalidation_reason;
        LeaveCriticalSection(&g_sapi_lock);

        if (task.playback_generation < activeGeneration) {
            const HRESULT purgeResult = purge_sapi_queue(voice);
            if (FAILED(purgeResult)) {
                speaklyrics_log_warning(L"SAPI：取消旧任务时清空语音失败，任务=%llu，HRESULT=0x%08lX。",
                    static_cast<unsigned long long>(task.task_id),
                    static_cast<unsigned long>(purgeResult));
            }
            finish_active_task(std::move(task), speech_task_result_status::canceled, activeReason);
            continue;
        }

        if (ok) {
            if (task.retry_attempt > 0) {
                speaklyrics_log_info(L"SAPI：%s任务=%llu 第%u次重试成功。",
                    speech_task_name(task.type), static_cast<unsigned long long>(task.task_id),
                    task.retry_attempt);
            }
            finish_active_task(std::move(task), speech_task_result_status::dispatched);
            continue;
        }

        if (speech_task_is_expired(task, GetTickCount64())) {
            finish_active_task(std::move(task), speech_task_result_status::expired);
            continue;
        }

        if (speech_task_can_retry(task) &&
            task.retry_attempt < static_cast<unsigned>(_countof(kRetryDelaysMs)) &&
            !speech_task_is_expired(task, GetTickCount64())) {
            const DWORD delay = kRetryDelaysMs[task.retry_attempt];
            ++task.retry_attempt;
            task.ready_at = GetTickCount64() + delay;
            speaklyrics_log_warning(L"SAPI：%s任务=%llu 调用失败，将在%lu毫秒后进行第%u次重试。",
                speech_task_name(task.type), static_cast<unsigned long long>(task.task_id),
                delay, task.retry_attempt);

            std::vector<speech_task> retryExpired;
            std::vector<speech_task_cancellation> retryCanceled;
            speech_enqueue_status retryStatus = speech_enqueue_status::shutting_down;
            EnterCriticalSection(&g_sapi_lock);
            if (!g_sapi_shutdown_requested &&
                g_sapi_worker_state == worker_lifecycle_state::running) {
                if (task.playback_generation < g_sapi_active_generation) {
                    retryStatus = speech_enqueue_status::stale_generation;
                    activeReason = g_sapi_last_invalidation_reason;
                } else {
                    retryStatus = g_sapi_tasks.requeue_retry(task, GetTickCount64(),
                        retryExpired, retryCanceled);
                    if (retryStatus == speech_enqueue_status::accepted) {
                        clear_active_task_locked(task.task_id);
                    }
                }
            }
            LeaveCriticalSection(&g_sapi_lock);
            report_expired_tasks(retryExpired);
            report_canceled_tasks(retryCanceled);
            if (retryStatus == speech_enqueue_status::accepted) continue;
            speaklyrics_log_warning(
                L"SAPI：%s任务=%llu 第%u次重试重新入队失败，原因=%s。",
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

        speaklyrics_log_warning(L"SAPI：%s任务=%llu 调用失败，已停止重试。",
            speech_task_name(task.type), static_cast<unsigned long long>(task.task_id));
        finish_active_task(std::move(task), speech_task_result_status::failed,
            speech_invalidation_reason::none, E_FAIL);
    }

    if (voice) {
        const HRESULT purgeResult = purge_sapi_queue(voice);
        if (FAILED(purgeResult)) {
            speaklyrics_log_warning(L"SAPI：工作线程退出时清空语音失败，HRESULT=0x%08lX。",
                static_cast<unsigned long>(purgeResult));
        }
    }
    if (voice) voice->Release();
    if (coInitialized) CoUninitialize();
    return 0;
}

bool ensure_worker() {
    init_lock_once();
    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_shutdown_requested ||
        g_sapi_worker_state == worker_lifecycle_state::stopping) {
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }
    if (g_sapi_worker_state == worker_lifecycle_state::running) {
        const bool ready = g_sapi_worker_context &&
            g_sapi_worker_context->event != nullptr &&
            g_sapi_worker_context->stopped_event != nullptr;
        LeaveCriticalSection(&g_sapi_lock);
        return ready;
    }

    worker_context_ptr context;
    try {
        context = std::make_shared<worker_context>();
    } catch (...) {
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }

    context->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!context->event) {
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }
    context->stopped_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!context->stopped_event) {
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }

    g_sapi_worker_context = context;
    g_sapi_worker_state = worker_lifecycle_state::running;
    try {
        // splitTask keeps the component loaded until the worker has returned.
        // The shared context keeps both event handles alive until every worker
        // and shutdown-side reference has been released.
        fb2k::splitTask([context]() {
            try {
                sapi_worker_proc(context);
            } catch (...) {
                speaklyrics_log_error(L"SAPI：语音工作线程发生未知异常。");
            }
            mark_worker_finished(context);
            if (!SetEvent(context->stopped_event)) {
                speaklyrics_log_error(L"SAPI：无法标记语音工作线程结束，错误码：%lu。", GetLastError());
            }
        });
    } catch (...) {
        g_sapi_worker_context.reset();
        g_sapi_worker_state = worker_lifecycle_state::stopped;
        LeaveCriticalSection(&g_sapi_lock);
        return false;
    }
    LeaveCriticalSection(&g_sapi_lock);
    return true;
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
    speech_task task;
    task.type = speech_task_type::general;
    task.text = text;
    task.interrupt = interrupt;
    task.voice_type = voiceType ? voiceType : L"sapi5";
    task.voice_id = voiceId ? voiceId : L"";
    task.voice_rate = rate;
    task.playback_generation = speech_current_playback_generation();
    task.queued_at = GetTickCount64();
    task.ready_at = task.queued_at;
    task.expires_at = task.queued_at + 5000;
    task.text_hash = speaklyrics_log_text_hash(task.text.c_str());
    return sapi_queue_task(std::move(task)).accepted();
}

speech_enqueue_result sapi_queue_task(speech_task task) {
    init_lock_once();
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
        EnterCriticalSection(&g_sapi_lock);
        result.status = g_sapi_shutdown_requested ? speech_enqueue_status::shutting_down :
            speech_enqueue_status::worker_unavailable;
        LeaveCriticalSection(&g_sapi_lock);
        return result;
    }

    std::vector<speech_task> expired;
    std::vector<speech_task_cancellation> canceled;
    DWORD signalError = ERROR_SUCCESS;
    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_shutdown_requested ||
        g_sapi_worker_state == worker_lifecycle_state::stopping) {
        result.status = speech_enqueue_status::shutting_down;
    } else if (g_sapi_worker_state != worker_lifecycle_state::running ||
        !g_sapi_worker_context) {
        result.status = speech_enqueue_status::worker_unavailable;
    } else if (task.playback_generation < g_sapi_active_generation) {
        result.status = speech_enqueue_status::stale_generation;
    } else {
        result.status = g_sapi_tasks.enqueue(std::move(task), GetTickCount64(), expired, canceled);
        result.queue_depth = g_sapi_tasks.size();
        if (result.accepted()) {
            if (!g_sapi_worker_context || !g_sapi_worker_context->event) {
                g_sapi_tasks.remove(result.task_id);
                result.status = speech_enqueue_status::worker_unavailable;
                result.queue_depth = g_sapi_tasks.size();
            } else if (!SetEvent(g_sapi_worker_context->event)) {
                signalError = GetLastError();
                g_sapi_tasks.remove(result.task_id);
                result.status = speech_enqueue_status::signal_failed;
                result.queue_depth = g_sapi_tasks.size();
            }
        }
    }
    LeaveCriticalSection(&g_sapi_lock);
    report_expired_tasks(expired);
    report_canceled_tasks(canceled);
    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"SAPI：任务=%llu 已撤回，无法通知工作线程，错误码：%lu。",
            static_cast<unsigned long long>(result.task_id), signalError);
    }
    return result;
}

void sapi_invalidate_pending(uint64_t activeGeneration, speech_invalidation_reason reason) {
    init_lock_once();
    std::vector<speech_task_cancellation> canceled;
    DWORD signalError = ERROR_SUCCESS;
    uint64_t activeTaskId = 0;
    bool workerAvailable = false;
    EnterCriticalSection(&g_sapi_lock);
    if (activeGeneration > g_sapi_active_generation) g_sapi_active_generation = activeGeneration;
    g_sapi_last_invalidation_reason = reason;
    g_sapi_tasks.invalidate_before_generation(g_sapi_active_generation, reason, canceled);
    if (g_sapi_has_active_task &&
        g_sapi_active_task.playback_generation < g_sapi_active_generation) {
        activeTaskId = g_sapi_active_task.task_id;
    }
    workerAvailable = g_sapi_worker_state == worker_lifecycle_state::running &&
        g_sapi_worker_context != nullptr;
    if (workerAvailable) {
        g_sapi_purge_requested = true;
        if (!SetEvent(g_sapi_worker_context->event)) signalError = GetLastError();
    }
    LeaveCriticalSection(&g_sapi_lock);

    report_canceled_tasks(canceled);
    if (workerAvailable || !canceled.empty() || activeTaskId != 0) {
        speaklyrics_log_info(
            L"SAPI：任务失效完成，播放世代=%llu，已取消等待任务=%llu，正在调用的旧任务=%llu，已请求清空 SAPI 队列。",
            static_cast<unsigned long long>(activeGeneration),
            static_cast<unsigned long long>(canceled.size()),
            static_cast<unsigned long long>(activeTaskId));
    }
    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"SAPI：无法通知工作线程执行失效和清空，错误码：%lu。", signalError);
    }
}

void sapi_shutdown() {
    init_lock_once();
    worker_context_ptr context;
    std::vector<speech_task_cancellation> canceled;
    bool should_signal = false;
    DWORD signalError = ERROR_SUCCESS;
    EnterCriticalSection(&g_sapi_lock);
    g_sapi_shutdown_requested = true;
    g_sapi_tasks.cancel_all(speech_invalidation_reason::shutdown, canceled);
    g_sapi_purge_requested = true;
    if (g_sapi_worker_state == worker_lifecycle_state::running && g_sapi_worker_context) {
        g_sapi_worker_state = worker_lifecycle_state::stopping;
        context = g_sapi_worker_context;
        should_signal = true;
    } else if (g_sapi_worker_state == worker_lifecycle_state::stopping) {
        // A previous shutdown may still be waiting for a blocked SAPI call.
        // Reuse the same context and wait again instead of abandoning cleanup.
        context = g_sapi_worker_context;
        should_signal = context != nullptr;
    }
    if (should_signal && context && !SetEvent(context->event)) {
        signalError = GetLastError();
    }
    LeaveCriticalSection(&g_sapi_lock);
    report_canceled_tasks(canceled);

    if (!context) return;

    if (signalError != ERROR_SUCCESS) {
        speaklyrics_log_error(L"SAPI：无法通知语音工作线程退出，错误码：%lu。", signalError);
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
                L"SAPI：语音工作线程在5秒内未退出；保留工作线程上下文并禁止重新启动，线程结束后将自动回收事件句柄。");
        } else {
            speaklyrics_log_error(
                L"SAPI：等待语音工作线程退出失败，错误码：%lu；保留工作线程上下文并禁止重新启动。",
                waitError);
        }
        return;
    }

    EnterCriticalSection(&g_sapi_lock);
    if (g_sapi_worker_context.get() == context.get()) {
        // mark_worker_finished() normally clears this before stopped_event is
        // signaled. Keep this as an idempotent fallback for an abnormal path.
        g_sapi_worker_context.reset();
        g_sapi_worker_state = worker_lifecycle_state::stopped;
        g_sapi_purge_requested = false;
    }
    LeaveCriticalSection(&g_sapi_lock);

    speaklyrics_log_info(L"SAPI：语音工作线程已安全停止。");
}
