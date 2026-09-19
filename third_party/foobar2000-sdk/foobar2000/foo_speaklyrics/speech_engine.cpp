#include "stdafx.h"
#include "speech_engine.h"

#include "config.h"
#include "sapi_speech.h"
#include "speaklyrics_log.h"
#include "tolk_bridge.h"

#include <deque>

namespace {

std::atomic_uint64_t g_speech_task_id{ 0 };
std::atomic_uint64_t g_playback_generation{ 1 };
std::atomic_int g_selected_backend{ -1 };
std::atomic_bool g_speech_shutdown_requested{ false };
CRITICAL_SECTION g_result_lock;
INIT_ONCE g_result_lock_once = INIT_ONCE_STATIC_INIT;
std::deque<speech_task_result> g_task_results;

void init_result_lock_once() {
    InitOnceExecuteOnce(&g_result_lock_once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&g_result_lock);
        return TRUE;
    }, nullptr, nullptr);
}

class result_lock_guard {
public:
    result_lock_guard() {
        init_result_lock_once();
        EnterCriticalSection(&g_result_lock);
    }

    ~result_lock_guard() {
        LeaveCriticalSection(&g_result_lock);
    }

    result_lock_guard(const result_lock_guard&) = delete;
    result_lock_guard& operator=(const result_lock_guard&) = delete;
};

const wchar_t* task_type_name(speech_task_type type) {
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

const wchar_t* backend_name(speech_backend_type backend) {
    return backend == speech_backend_type::sapi ? L"SAPI5.1" : L"Tolk";
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

const wchar_t* result_status_name(speech_task_result_status status) {
    switch (status) {
    case speech_task_result_status::dispatched: return L"已提交给语音接口";
    case speech_task_result_status::failed: return L"调用失败";
    case speech_task_result_status::expired: return L"已经过期";
    case speech_task_result_status::canceled: return L"已取消";
    default: return L"未知";
    }
}

const wchar_t* invalidation_reason_name(speech_invalidation_reason reason) {
    switch (reason) {
    case speech_invalidation_reason::track_change: return L"切换歌曲";
    case speech_invalidation_reason::playback_stop: return L"停止播放";
    case speech_invalidation_reason::playback_pause: return L"暂停播放";
    case speech_invalidation_reason::playback_seek: return L"播放位置跳转";
    case speech_invalidation_reason::auto_speak_disabled: return L"关闭自动朗读";
    case speech_invalidation_reason::lyrics_reload: return L"歌词重载";
    case speech_invalidation_reason::metadata_change: return L"歌曲标签更新";
    case speech_invalidation_reason::same_title_lyrics: return L"切换同名歌词";
    case speech_invalidation_reason::silence: return L"静音";
    case speech_invalidation_reason::shutdown: return L"组件退出";
    case speech_invalidation_reason::backend_switch: return L"切换语音输出方式";
    case speech_invalidation_reason::superseded_by_lyric: return L"歌词优先于普通提示";
    case speech_invalidation_reason::queue_pressure: return L"队列容量不足";
    case speech_invalidation_reason::worker_exit: return L"工作线程退出";
    case speech_invalidation_reason::none:
    default: return L"无";
    }
}

std::wstring cfg_to_wide_speech(cfg_string& value) {
    pfc::string8 text = value.get();
    return pfc::stringcvt::string_wide_from_utf8(text).get_ptr();
}

speech_task make_task(speech_task_type type, const wchar_t* text, bool interrupt, unsigned validMs) {
    speech_task task;
    task.task_id = speech_allocate_task_id();
    task.type = type;
    task.text = text ? text : L"";
    task.interrupt = interrupt;
    task.playback_generation = g_playback_generation.load();
    task.queued_at = GetTickCount64();
    task.ready_at = task.queued_at;
    if (validMs > 0) task.expires_at = task.queued_at + validMs;
    task.text_hash = speaklyrics_log_text_hash(task.text.c_str());
    return task;
}

speech_enqueue_result enqueue_task(speech_task task) {
    speech_enqueue_result result;
    result.task_id = task.task_id;
    result.playback_generation = task.playback_generation;
    result.expires_at = task.expires_at;

    if (g_speech_shutdown_requested.load()) {
        result.status = speech_enqueue_status::shutting_down;
        return result;
    }

    const bool useScreenReader = cfg_use_screen_reader.get();
    if (!useScreenReader) {
        task.voice_type = L"sapi5";
        task.voice_id = cfg_to_wide_speech(cfg_tts_voice_id);
        task.voice_rate = static_cast<int>(cfg_tts_rate.get());
        return sapi_queue_task(std::move(task));
    }
    return tolk_queue_task(std::move(task));
}

void log_enqueue_result(const speech_task& task, const speech_enqueue_result& result,
    const wchar_t* target) {
    const std::wstring excerpt = speaklyrics_log_text_excerpt(task.text.c_str());
    if (result.accepted()) {
        speaklyrics_log_info(
            L"语音任务已入队：任务=%llu，类型=%s，目标=%s，歌曲会话=%llu，歌词文档=%llu，播放世代=%llu，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，播放时间=%d，队列深度=%llu，文本哈希=%016llX，内容=%s。",
            static_cast<unsigned long long>(task.task_id), task_type_name(task.type), target,
            static_cast<unsigned long long>(task.track_session),
            static_cast<unsigned long long>(task.document_generation),
            static_cast<unsigned long long>(task.playback_generation),
            static_cast<unsigned long long>(task.position_epoch),
            task.line_index, static_cast<unsigned long long>(task.line_id),
            static_cast<unsigned long long>(task.line_ids.size()), task.lyric_time_ms,
            task.trigger_time_ms, task.playback_time_ms,
            static_cast<unsigned long long>(result.queue_depth),
            static_cast<unsigned long long>(task.text_hash), excerpt.c_str());
    } else {
        speaklyrics_log_warning(
            L"语音任务入队失败：任务=%llu，类型=%s，目标=%s，歌曲会话=%llu，歌词文档=%llu，播放世代=%llu，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，原因=%s，文本哈希=%016llX，内容=%s。",
            static_cast<unsigned long long>(task.task_id), task_type_name(task.type), target,
            static_cast<unsigned long long>(task.track_session),
            static_cast<unsigned long long>(task.document_generation),
            static_cast<unsigned long long>(task.playback_generation),
            static_cast<unsigned long long>(task.position_epoch),
            task.line_index, static_cast<unsigned long long>(task.line_id),
            static_cast<unsigned long long>(task.line_ids.size()), task.lyric_time_ms,
            task.trigger_time_ms, enqueue_status_name(result.status),
            static_cast<unsigned long long>(task.text_hash), excerpt.c_str());
    }
}

speech_enqueue_result queue_simple_task(speech_task_type type, const wchar_t* text,
    bool interrupt, unsigned validMs) {
    speech_enqueue_result invalid;
    if (type != speech_task_type::preload && (!text || !*text)) {
        invalid.status = speech_enqueue_status::invalid_input;
        return invalid;
    }

    speech_task task = make_task(type, text, interrupt, validMs);
    const bool useScreenReader = cfg_use_screen_reader.get();
    const wchar_t* target = useScreenReader ? L"Tolk" : L"SAPI5.1";
    speech_enqueue_result result = enqueue_task(task);
    log_enqueue_result(task, result, target);
    return result;
}

} // namespace

bool speech_speak(const wchar_t* text, bool interrupt) {
    if (!cfg_use_screen_reader.get()) {
        std::wstring voiceId = cfg_to_wide_speech(cfg_tts_voice_id);
        return sapi_speak(L"sapi5", voiceId.c_str(), static_cast<int>(cfg_tts_rate.get()), text, interrupt);
    }
    return tolk_speak(text, interrupt);
}

void speech_preload() {
    const int selectedBackend = cfg_use_screen_reader.get() ? 0 : 1;
    const int previousBackend = g_selected_backend.exchange(selectedBackend);
    if (previousBackend >= 0 && previousBackend != selectedBackend) {
        speech_invalidate_pending(speech_invalidation_reason::backend_switch);
    }
    queue_simple_task(speech_task_type::preload, L"", false, 2500);
}

void speech_queue_speak(const wchar_t* text, bool interrupt) {
    queue_simple_task(speech_task_type::general, text, interrupt, 5000);
}

void speech_queue_track_announcement(const wchar_t* text, bool interrupt) {
    queue_simple_task(speech_task_type::track_announcement, text, interrupt, 10000);
}

speech_enqueue_result speech_queue_lyric(const wchar_t* text, bool interrupt, unsigned validMs,
    const lyric_speech_diagnostic_context& diagnostic) {
    speech_enqueue_result invalid;
    if (!text || !*text) {
        invalid.status = speech_enqueue_status::invalid_input;
        return invalid;
    }

    speech_task task = make_task(speech_task_type::lyric, text, interrupt, validMs);
    task.track_session = diagnostic.track_session;
    task.document_generation = diagnostic.document_generation;
    task.line_index = diagnostic.line_index;
    task.line_id = diagnostic.line_id;
    task.line_ids = diagnostic.line_ids;
    if (task.line_ids.empty() && task.line_id != 0) task.line_ids.push_back(task.line_id);
    task.position_epoch = diagnostic.position_epoch;
    task.lyric_time_ms = diagnostic.lyric_time_ms;
    task.trigger_time_ms = diagnostic.trigger_time_ms;
    task.playback_time_ms = diagnostic.playback_time_ms;

    const int elapsedMs = (std::max)(0, diagnostic.playback_time_ms - diagnostic.lyric_time_ms);
    if (validMs > 0) {
        if (elapsedMs >= static_cast<int>(validMs)) {
            invalid.status = speech_enqueue_status::expired;
            invalid.task_id = task.task_id;
            invalid.playback_generation = task.playback_generation;
            invalid.expires_at = task.queued_at;
            log_enqueue_result(task, invalid, cfg_use_screen_reader.get() ? L"Tolk" : L"SAPI5.1");
            return invalid;
        }
        task.expires_at = task.queued_at + (validMs - static_cast<unsigned>(elapsedMs));
    }

    const bool useScreenReader = cfg_use_screen_reader.get();
    const wchar_t* target = useScreenReader ? L"Tolk" : L"SAPI5.1";
    speaklyrics_log_info(
        L"语音任务已计划：任务=%llu，类型=歌词，目标=%s，歌曲会话=%llu，歌词文档=%llu，播放世代=%llu，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，播放时间=%d，有效至=%llu，文本哈希=%016llX。",
        static_cast<unsigned long long>(task.task_id), target,
        static_cast<unsigned long long>(task.track_session),
        static_cast<unsigned long long>(task.document_generation),
        static_cast<unsigned long long>(task.playback_generation),
        static_cast<unsigned long long>(task.position_epoch),
        task.line_index, static_cast<unsigned long long>(task.line_id),
        static_cast<unsigned long long>(task.line_ids.size()), task.lyric_time_ms,
        task.trigger_time_ms, task.playback_time_ms,
        static_cast<unsigned long long>(task.expires_at),
        static_cast<unsigned long long>(task.text_hash));

    speech_enqueue_result result = enqueue_task(task);
    log_enqueue_result(task, result, target);
    return result;
}

void speech_queue_auto_speak_state(bool enabled) {
    const wchar_t* text = enabled ? L"LRC朗读已打开" : L"LRC朗读已关闭";
    if (!enabled) {
        speech_invalidate_pending(speech_invalidation_reason::auto_speak_disabled);
    }
    queue_simple_task(enabled ? speech_task_type::auto_speak_enabled :
        speech_task_type::auto_speak_disabled, text, true, 2500);
}

void speech_queue_silence() {
    speech_invalidate_pending(speech_invalidation_reason::silence);
}

uint64_t speech_allocate_task_id() {
    return ++g_speech_task_id;
}

uint64_t speech_invalidate_pending(speech_invalidation_reason reason) {
    const uint64_t generation = g_playback_generation.fetch_add(1) + 1;
    speaklyrics_log_info(L"语音任务统一失效：新播放世代=%llu，原因=%s。",
        static_cast<unsigned long long>(generation), invalidation_reason_name(reason));
    tolk_invalidate_pending(generation, reason);
    sapi_invalidate_pending(generation, reason);
    return generation;
}

uint64_t speech_current_playback_generation() {
    return g_playback_generation.load();
}

void speech_report_task_result(speech_task_result result) {
    if (result.completed_at == 0) result.completed_at = GetTickCount64();
    const std::wstring excerpt = speaklyrics_log_text_excerpt(result.task.text.c_str());
    const wchar_t* status = result_status_name(result.status);
    const wchar_t* reason = invalidation_reason_name(result.reason);

    if (result.status == speech_task_result_status::failed ||
        result.status == speech_task_result_status::expired) {
        speaklyrics_log_warning(
            L"语音任务结果：任务=%llu，类型=%s，目标=%s，状态=%s，原因=%s，错误=%lld，歌曲会话=%llu，歌词文档=%llu，播放世代=%llu，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，文本哈希=%016llX，内容=%s。",
            static_cast<unsigned long long>(result.task.task_id), task_type_name(result.task.type),
            backend_name(result.backend), status, reason, static_cast<long long>(result.error_code),
            static_cast<unsigned long long>(result.task.track_session),
            static_cast<unsigned long long>(result.task.document_generation),
            static_cast<unsigned long long>(result.task.playback_generation),
            static_cast<unsigned long long>(result.task.position_epoch),
            result.task.line_index, static_cast<unsigned long long>(result.task.line_id),
            static_cast<unsigned long long>(result.task.line_ids.size()),
            result.task.lyric_time_ms, result.task.trigger_time_ms,
            static_cast<unsigned long long>(result.task.text_hash), excerpt.c_str());
    } else {
        speaklyrics_log_info(
            L"语音任务结果：任务=%llu，类型=%s，目标=%s，状态=%s，原因=%s，歌曲会话=%llu，歌词文档=%llu，播放世代=%llu，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，文本哈希=%016llX，内容=%s。",
            static_cast<unsigned long long>(result.task.task_id), task_type_name(result.task.type),
            backend_name(result.backend), status, reason,
            static_cast<unsigned long long>(result.task.track_session),
            static_cast<unsigned long long>(result.task.document_generation),
            static_cast<unsigned long long>(result.task.playback_generation),
            static_cast<unsigned long long>(result.task.position_epoch),
            result.task.line_index, static_cast<unsigned long long>(result.task.line_id),
            static_cast<unsigned long long>(result.task.line_ids.size()),
            result.task.lyric_time_ms, result.task.trigger_time_ms,
            static_cast<unsigned long long>(result.task.text_hash), excerpt.c_str());
    }

    if (speech_task_is_lyric(result.task)) {
        result_lock_guard guard;
        g_task_results.push_back(std::move(result));
    }
}

std::vector<speech_task_result> speech_drain_task_results() {
    std::vector<speech_task_result> results;
    result_lock_guard guard;
    results.reserve(g_task_results.size());
    while (!g_task_results.empty()) {
        results.push_back(std::move(g_task_results.front()));
        g_task_results.pop_front();
    }
    return results;
}

void speech_shutdown() {
    if (g_speech_shutdown_requested.exchange(true)) return;
    speech_invalidate_pending(speech_invalidation_reason::shutdown);
    sapi_shutdown();
    tolk_shutdown();
}
