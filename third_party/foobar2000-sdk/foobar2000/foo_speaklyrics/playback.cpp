#include "stdafx.h"

#include "config.h"

#include "background_task.h"
#include "embedded_lyrics_selector.h"
#include "filesystem_safety.h"
#include "process_runner.h"

#include "lrc_download_retry.h"
#include "lrc_parser.h"
#include "playback.h"
#include "lyrics_copy.h"
#include "lyrics_jump_window.h"

#include "speech_engine.h"
#include "speaklyrics_log.h"
#include "temp_lrc_manifest.h"

#include <unordered_set>



namespace {

lrc_document g_doc;

uint64_t g_last_scheduled_line_id = 0;
uint64_t g_last_dispatched_line_id = 0;
uint64_t g_last_dispatched_task_id = 0;
int g_scheduler_scan_trigger_ms = -1;
uint64_t g_lyric_position_epoch = 1;

std::atomic_bool g_same_title_switch_in_progress{ false };
int g_same_title_candidate_index = 0;
std::wstring g_same_title_candidate_cache_path;
std::wstring g_same_title_candidate_cache_key;
std::wstring g_same_title_prefetch_requested_key;
speaklyrics_background_task_ptr g_same_title_prefetch_task;
speaklyrics_background_task_ptr g_lrc_downloader_task;
speaklyrics_background_task_ptr g_same_title_switch_task;

bool g_paused = false;

std::wstring g_current_lrc;

bool g_current_lrc_temporary = false;

std::wstring g_manual_lrc_track_key;

ULONGLONG g_last_missing_lrc_scan_tick = 0;

lrc_download_retry_state g_lrc_download_state;

std::wstring g_current_track_key;

std::wstring g_loaded_lrc_track_key;

struct lyric_match_fingerprint {
    bool info_available = false;
    std::wstring track_key;
    t_uint32 subsong_index = 0;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    int duration_seconds = 0;
    std::vector<std::wstring> embedded_lyrics;
};

lyric_match_fingerprint g_lyric_match_fingerprint;
bool g_ignored_metadata_update_logged = false;

uint64_t g_track_session_id = 0;
uint64_t g_document_generation = 0;
double g_last_playback_callback_time = -1.0;
bool g_pending_announcement_skip_logged = false;
uint64_t g_last_skip_document_generation = 0;
uint64_t g_last_skip_line_id = 0;
int g_last_skip_reason = 0;

struct lyric_submission_record {
    uint64_t track_session = 0;
    uint64_t document_generation = 0;
    uint64_t playback_generation = 0;
    uint64_t speech_task_id = 0;
    int line_index = -1;
    uint64_t line_id = 0;
    uint64_t position_epoch = 0;
    int lyric_time_ms = 0;
    int trigger_time_ms = 0;
    uint64_t text_hash = 0;
    ULONGLONG submitted_at = 0;
    ULONGLONG expires_at = 0;
    std::vector<uint64_t> line_ids;
};

lyric_submission_record g_last_lyric_submission;
std::vector<lyric_submission_record> g_pending_lyric_submissions;
std::vector<lyric_submission_record> g_recent_lyric_submissions;
std::unordered_set<uint64_t> g_scheduled_line_ids;
std::unordered_set<uint64_t> g_dispatched_line_ids;
std::unordered_set<uint64_t> g_skipped_line_ids;
std::unordered_set<uint64_t> g_retry_line_ids;
std::unordered_set<std::wstring> g_filesystem_error_log_keys;

struct track_diagnostic_counters {
    uint64_t planned = 0;
    uint64_t accepted = 0;
    uint64_t dispatched = 0;
    uint64_t failed = 0;
    uint64_t task_expired = 0;
    uint64_t canceled = 0;
    uint64_t rejected = 0;
    uint64_t suspected_duplicates = 0;
    uint64_t crossed_lines = 0;
    uint64_t expired_lines = 0;
    uint64_t delayed_callbacks = 0;
    uint64_t announcement_blocks = 0;
};

track_diagnostic_counters g_track_diagnostics;
bool g_track_summary_logged = false;

struct loaded_lrc_snapshot {
    bool valid = false;
    std::wstring track_key;
    uint64_t document_hash = 0;
    std::vector<uint64_t> dispatched_line_ids;
    uint64_t last_dispatched_line_id = 0;
    uint64_t last_dispatched_task_id = 0;
};

std::wstring g_pending_track_announce_text;

ULONGLONG g_pending_track_announce_due_tick = 0;

struct pending_temp_lrc_delete {
    std::wstring path;
    ULONGLONG due_tick = 0;
};

std::vector<pending_temp_lrc_delete> g_pending_temp_lrc_deletes;

void cancel_pending_temp_lrc_delete(const std::wstring& path);
void schedule_current_temp_lrc_delete();
void reset_last_spoken(const wchar_t* reason);
void advance_lyric_position_epoch(const wchar_t* reason);
void mark_document_loaded(const wchar_t* source);
loaded_lrc_snapshot capture_loaded_lrc_snapshot();
void restore_last_spoken_if_same_lyrics(const loaded_lrc_snapshot& snapshot, metadb_handle_ptr track,
    const wchar_t* source);
void process_speech_task_results();



struct lrc_match {

    std::wstring path;

    bool temporary;

    std::wstring embedded_text;

};



std::wstring utf8_to_wide(const char* s) {

    return pfc::stringcvt::string_wide_from_utf8(s ? s : "").get_ptr();

}



std::wstring cfg_path_wide(cfg_string& var) {

    return expand_environment_path(utf8_to_wide(var.get().c_str()));

}



std::optional<std::wstring> local_track_path(metadb_handle_ptr track) {

    if (track.is_empty()) return std::nullopt;

    pfc::string8 native;

    if (foobar2000_io::extract_native_path_archive_aware(track->get_path(), native)) {

        return utf8_to_wide(native.c_str());

    }

    return std::nullopt;

}



std::wstring track_key(metadb_handle_ptr track) {

    if (track.is_empty()) return std::wstring();

    return utf8_to_wide(track->get_path());

}



std::wstring current_dll_dir() {

    pfc::string8 path = core_api::get_my_full_path();

    std::wstring wide = pfc::stringcvt::string_wide_from_utf8(path).get_ptr();

    size_t slash = wide.find_last_of(L"\\/");

    return slash == std::wstring::npos ? L"" : wide.substr(0, slash);

}



std::wstring command_line_quote(const std::wstring& value) {

    std::wstring quoted = L"\"";

    size_t backslashes = 0;

    for (wchar_t ch : value) {

        if (ch == L'\\') {

            ++backslashes;

        } else if (ch == L'\"') {

            quoted.append(backslashes * 2 + 1, L'\\');

            quoted.push_back(ch);

            backslashes = 0;

        } else {

            quoted.append(backslashes, L'\\');

            backslashes = 0;

            quoted.push_back(ch);

        }

    }

    quoted.append(backslashes * 2, L'\\');

    quoted.push_back(L'\"');

    return quoted;

}

void log_filesystem_error_once(const wchar_t* source, const wchar_t* operation,
    const fs::path& path, const std::error_code& error) noexcept {
    try {
        const wchar_t* safeSource = source ? source : L"未知来源";
        const wchar_t* safeOperation = operation ? operation : L"文件系统操作";
        std::wstring key(safeSource);
        key.push_back(L'|');
        key += safeOperation;
        key.push_back(L'|');
        key += path.native();
        key.push_back(L'|');
        key += std::to_wstring(error.value());
        if (!g_filesystem_error_log_keys.insert(std::move(key)).second) return;

        speaklyrics_log_warning(
            L"文件系统访问失败：来源=%s，操作=%s，系统错误码=%d，路径=%s。",
            safeSource, safeOperation, error.value(), path.c_str());
    } catch (...) {
        // A diagnostic failure must never replace the original filesystem failure.
    }
}

bool safe_filesystem_exists(const fs::path& path, const wchar_t* source) noexcept {
    const auto result = speaklyrics_filesystem::exists(path);
    if (result.error) {
        log_filesystem_error_once(source, L"检查路径是否存在", path, result.error);
    }
    return result.value && !result.error;
}

bool safe_filesystem_is_directory(const fs::path& path,
    const wchar_t* source) noexcept {
    const auto result = speaklyrics_filesystem::is_directory(path);
    if (result.error) {
        log_filesystem_error_once(source, L"检查目录", path, result.error);
    }
    return result.value && !result.error;
}

bool safe_filesystem_is_regular_file(const fs::path& path,
    const wchar_t* source) noexcept {
    const auto result = speaklyrics_filesystem::is_regular_file(path);
    if (result.error) {
        log_filesystem_error_once(source, L"检查普通文件", path, result.error);
    }
    return result.value && !result.error;
}

bool safe_filesystem_open_directory(const fs::path& path, const wchar_t* source,
    fs::directory_iterator& iterator) noexcept {
    const std::error_code error =
        speaklyrics_filesystem::open_directory(path, iterator);
    if (!error) return true;
    log_filesystem_error_once(source, L"打开目录进行枚举", path, error);
    return false;
}

bool safe_filesystem_increment_directory(fs::directory_iterator& iterator,
    const fs::path& path, const wchar_t* source) noexcept {
    const std::error_code error =
        speaklyrics_filesystem::increment_directory(iterator);
    if (!error) return true;
    log_filesystem_error_once(source, L"继续枚举目录", path, error);
    return false;
}

void log_filesystem_boundary_failure(const wchar_t* source,
    const wchar_t* pathText, const std::error_code& error) noexcept {
    try {
        log_filesystem_error_once(source, L"歌词查找异常边界",
            fs::path(pathText ? pathText : L""),
            error ? error : std::make_error_code(std::errc::io_error));
    } catch (...) {
        // The exception boundary itself must remain nonthrowing.
    }
}

void parse_candidate_downloader_output(const std::string& output, std::wstring& path, std::wstring& title, std::wstring& artist) {
    std::wstring text = utf8_to_wide(output.c_str());
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (line.rfind(L"SELECTED:\t", 0) == 0) {
            size_t titleStart = 10;
            size_t artistStart = line.find(L'\t', titleStart);
            title = line.substr(titleStart, artistStart == std::wstring::npos ? std::wstring::npos : artistStart - titleStart);
            if (artistStart != std::wstring::npos) artist = line.substr(artistStart + 1);
        } else if (line.size() >= 4 && _wcsicmp(line.c_str() + line.size() - 4, L".lrc") == 0) {
            path = line;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start < text.size() && text[start - 1] == L'\r' && text[start] == L'\n') ++start;
    }
}



std::wstring meta_value(const file_info_impl& info, const char* name) {

    const char* value = info.meta_get(name, 0);

    return value && *value ? utf8_to_wide(value) : std::wstring();

}

lyric_match_fingerprint make_lyric_match_fingerprint(metadb_handle_ptr track) {
    lyric_match_fingerprint fingerprint;
    if (track.is_empty()) return fingerprint;

    fingerprint.track_key = track_key(track);
    fingerprint.subsong_index = track->get_subsong_index();

    file_info_impl info;
    // on_playback_edited() can arrive while the fresh metadb hint is still
    // pending. Prefer the async snapshot so title, artist and LYRICS changes
    // are compared against the newly edited values instead of stale cache.
    fingerprint.info_available = track->get_info_async(info);
    if (!fingerprint.info_available) {
        fingerprint.info_available = track->get_info(info);
    }
    if (!fingerprint.info_available) return fingerprint;

    fingerprint.title = meta_value(info, "title");
    fingerprint.artist = meta_value(info, "artist");
    fingerprint.album = meta_value(info, "album");

    const double length = info.get_length();
    if (length > 0) fingerprint.duration_seconds = static_cast<int>(length + 0.5);

    const t_size lyricValueCount = info.meta_get_count_by_name("LYRICS");
    fingerprint.embedded_lyrics.reserve(static_cast<size_t>(lyricValueCount));
    for (t_size index = 0; index < lyricValueCount; ++index) {
        const char* value = info.meta_get("LYRICS", index);
        fingerprint.embedded_lyrics.push_back(utf8_to_wide(value ? value : ""));
    }

    return fingerprint;
}

enum lyric_match_field_mask : unsigned {
    lyric_match_field_none = 0,
    lyric_match_field_path = 1u << 0,
    lyric_match_field_subsong = 1u << 1,
    lyric_match_field_title = 1u << 2,
    lyric_match_field_artist = 1u << 3,
    lyric_match_field_album = 1u << 4,
    lyric_match_field_duration = 1u << 5,
    lyric_match_field_embedded_lyrics = 1u << 6,
};

unsigned lyric_match_fingerprint_difference(const lyric_match_fingerprint& previous,
    const lyric_match_fingerprint& current) {
    unsigned differences = lyric_match_field_none;
    if (previous.track_key != current.track_key) differences |= lyric_match_field_path;
    if (previous.subsong_index != current.subsong_index) differences |= lyric_match_field_subsong;
    // A transiently unavailable metadata cache must not look like the user
    // cleared every relevant field. Wait for the next edit notification when
    // the current snapshot cannot be read.
    if (current.info_available) {
        if (!previous.info_available || previous.title != current.title) differences |= lyric_match_field_title;
        if (!previous.info_available || previous.artist != current.artist) differences |= lyric_match_field_artist;
        if (!previous.info_available || previous.album != current.album) differences |= lyric_match_field_album;
        if (!previous.info_available || previous.duration_seconds != current.duration_seconds) differences |= lyric_match_field_duration;
        if (!previous.info_available || previous.embedded_lyrics != current.embedded_lyrics) {
            differences |= lyric_match_field_embedded_lyrics;
        }
    }
    return differences;
}

std::wstring lyric_match_field_names(unsigned differences) {
    std::wstring names;
    const auto append = [&names](const wchar_t* name) {
        if (!names.empty()) names += L"、";
        names += name;
    };

    if (differences & lyric_match_field_path) append(L"路径");
    if (differences & lyric_match_field_subsong) append(L"子曲目");
    if (differences & lyric_match_field_title) append(L"标题");
    if (differences & lyric_match_field_artist) append(L"艺术家");
    if (differences & lyric_match_field_album) append(L"专辑");
    if (differences & lyric_match_field_duration) append(L"时长");
    if (differences & lyric_match_field_embedded_lyrics) append(L"内嵌LYRICS");

    return names.empty() ? L"未知" : names;
}

std::wstring trim_text(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

std::wstring lowercase_text(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return text;
}

bool looks_like_download_source_artist(const std::wstring& artist) {
    const std::wstring normalized = lowercase_text(trim_text(artist));
    if (normalized.empty()) return false;

    static const wchar_t* const markers[] = {
        L"伴奏网", L"伴奏网站", L"伴奏下载", L"立体声伴奏",
        L"音乐下载", L"歌曲下载", L"音乐网", L"音乐网站",
        L"资源网", L"铃声网", L"mp3", L"www.", L"http://", L"https://",
        L".com", L".net", L".cn"
    };
    for (const wchar_t* marker : markers) {
        if (normalized.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

bool split_combined_artist_title(const std::wstring& value, std::wstring& artist, std::wstring& title) {
    static const wchar_t separators[] = { L'-', L'\uFF0D', L'\u2013', L'\u2014' };
    for (wchar_t separator : separators) {
        size_t start = 0;
        while (start < value.size()) {
            const size_t at = value.find(separator, start);
            if (at == std::wstring::npos) break;
            std::wstring left = trim_text(value.substr(0, at));
            std::wstring right = trim_text(value.substr(at + 1));
            if (!left.empty() && !right.empty()) {
                artist = std::move(left);
                title = std::move(right);
                return true;
            }
            start = at + 1;
        }
    }
    return false;
}

bool same_metadata_text(const std::wstring& first, const std::wstring& second) {
    const std::wstring a = lowercase_text(trim_text(first));
    const std::wstring b = lowercase_text(trim_text(second));
    return !a.empty() && a == b;
}



struct downloader_track_info {

    std::wstring title;

    std::wstring artist;

    std::wstring album;

    int duration_seconds = 0;

    bool metadata_corrected = false;

    std::wstring original_title;

    std::wstring original_artist;

};



downloader_track_info get_downloader_track_info(metadb_handle_ptr track) {

    downloader_track_info out;

    if (track.is_empty()) return out;



    file_info_impl info;

    if (track->get_info(info)) {

        out.title = meta_value(info, "title");

        out.artist = meta_value(info, "artist");

        out.album = meta_value(info, "album");

        double length = info.get_length();

        if (length > 0) out.duration_seconds = static_cast<int>(length + 0.5);

    }



    if (out.title.empty()) {

        if (auto path = local_track_path(track)) {

            out.title = fs::path(*path).stem().wstring();

        }

    }

    // Some accompaniment files keep the real song title only in the file name.
    // Treat either a three-field placeholder collision, or a source-site title
    // with no artist, as strong evidence that the local file name is better.
    const bool repeatedPlaceholder = same_metadata_text(out.title, out.artist) && same_metadata_text(out.title, out.album);
    const bool sourceTitleWithoutArtist = out.artist.empty() && looks_like_download_source_artist(out.title);
    if (repeatedPlaceholder || sourceTitleWithoutArtist) {
        if (auto path = local_track_path(track)) {
            std::wstring fileTitle = trim_text(fs::path(*path).stem().wstring());
            if (!fileTitle.empty() && !same_metadata_text(fileTitle, out.title)) {
                out.original_title = out.title;
                out.original_artist = out.artist;
                out.title = std::move(fileTitle);
                out.artist.clear();
                out.album.clear();
                out.metadata_corrected = true;
            }
        }
    }

    if (!out.metadata_corrected && looks_like_download_source_artist(out.artist)) {
        std::wstring correctedArtist;
        std::wstring correctedTitle;
        if (split_combined_artist_title(out.title, correctedArtist, correctedTitle)) {
            out.original_title = out.title;
            out.original_artist = out.artist;
            out.title = std::move(correctedTitle);
            out.artist = std::move(correctedArtist);
            out.metadata_corrected = true;
        }
    }



    return out;

}

void clear_same_title_candidate_cache() {
    if (g_same_title_prefetch_task) {
        g_same_title_prefetch_task->cancel();
        g_same_title_prefetch_task.reset();
    }
    if (!g_same_title_candidate_cache_path.empty()) {
        std::error_code error;
        fs::remove(g_same_title_candidate_cache_path, error);
    }
    g_same_title_candidate_cache_path.clear();
    g_same_title_candidate_cache_key.clear();
    g_same_title_prefetch_requested_key.clear();
}

std::wstring same_title_candidate_cache_path(const std::wstring& key) {
    if (key.empty()) return std::wstring();
    if (g_same_title_candidate_cache_key == key && !g_same_title_candidate_cache_path.empty()) {
        return g_same_title_candidate_cache_path;
    }

    clear_same_title_candidate_cache();
    std::error_code error;
    fs::path folder = fs::temp_directory_path(error);
    if (error) {
        log_filesystem_error_once(L"同名歌词候选缓存", L"获取系统临时目录",
            fs::path(), error);
        return std::wstring();
    }
    if (folder.empty()) return std::wstring();
    const size_t keyHash = std::hash<std::wstring>{}(key);
    fs::path path = folder / (L"foo_speaklyrics-candidates-" + std::to_wstring(static_cast<unsigned long long>(keyHash)) + L".json");
    g_same_title_candidate_cache_key = key;
    g_same_title_candidate_cache_path = path.wstring();
    return g_same_title_candidate_cache_path;
}

void maybe_prefetch_same_title_candidates(metadb_handle_ptr track) {
    if (track.is_empty()) return;
    downloader_track_info info = get_downloader_track_info(track);

    const std::wstring key = track_key(track);
    if (key.empty() || g_same_title_prefetch_requested_key == key) return;
    const std::wstring cachePath = same_title_candidate_cache_path(key);
    const std::wstring sources = cfg_path_wide(cfg_lyric_sources);
    fs::path exePath = fs::path(current_dll_dir()) / L"downloader" / L"LrcDownloader.exe";
    if (cachePath.empty() || sources.empty() || info.title.empty() ||
        !safe_filesystem_exists(exePath, L"同名歌词候选预取下载器")) return;

    g_same_title_prefetch_requested_key = key;
    // Same-title switching is intentionally title-only. Do not pass the
    // current artist to the downloader as a query restriction.
    std::wstring command = command_line_quote(exePath.wstring()) +
        L" --title " + command_line_quote(info.title) +
        L" --album " + command_line_quote(info.album) +
        L" --duration " + std::to_wstring(info.duration_seconds) +
        L" --sources " + command_line_quote(sources) +
        L" --search-only --title-only --list --candidate-cache " + command_line_quote(cachePath);

    speaklyrics_log_info(
        L"同名歌词候选：开始按标题预取，标题：%s，当前艺术家：%s；艺术家不作为候选的硬匹配条件。",
        info.title.c_str(), info.artist.c_str());

    auto task = speaklyrics_start_background_task(L"同名歌词候选预取");
    if (!task) {
        speaklyrics_log_warning(L"Background task skipped during shutdown.");
        return;
    }
    g_same_title_prefetch_task = task;
    const uint64_t session = g_track_session_id;
    speaklyrics_run_background_task(task,
        [task, command, exePath, key, cachePath, session](speaklyrics_background_task& background) {
        const speaklyrics_process_result result = run_process_capture_stdout(
            command, exePath.parent_path(), background.aborter(), 30000, 1024 * 1024);
        background.post_to_main_thread([task, key, cachePath, session, result]() {
            if (g_same_title_prefetch_task.get() == task.get()) {
                g_same_title_prefetch_task.reset();
            }
            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != key || g_current_track_key != key ||
                g_track_session_id != session) {
                std::error_code error;
                fs::remove(cachePath, error);
                return;
            }
            if (result.status == speaklyrics_process_status::completed &&
                result.exit_code == 0 && safe_filesystem_exists(
                    fs::path(cachePath), L"同名歌词候选缓存")) {
                speaklyrics_log_info(L"同名歌词候选：已完成后台预取。");
            } else {
                speaklyrics_log_warning(
                    L"同名歌词候选：后台预取未完成，状态=%s，退出码=%lu，错误码=%lu。",
                    speaklyrics_process_status_name(result.status), result.exit_code,
                    result.error_code);
            }
        });
    });
}

std::wstring fallback_track_path_text(metadb_handle_ptr track) {
    if (track.is_empty()) return std::wstring();
    return utf8_to_wide(track->get_path());
}

std::wstring track_file_name(metadb_handle_ptr track, bool withoutExtension) {
    if (auto path = local_track_path(track)) {
        fs::path file(*path);
        return withoutExtension ? file.stem().wstring() : file.filename().wstring();
    }
    std::wstring pathText = fallback_track_path_text(track);
    if (pathText.empty()) return std::wstring();
    fs::path file(pathText);
    std::wstring name = withoutExtension ? file.stem().wstring() : file.filename().wstring();
    return name.empty() ? pathText : name;
}

std::wstring join_nonempty(const std::wstring& first, const std::wstring& second) {
    std::wstring a = trim_text(first);
    std::wstring b = trim_text(second);
    if (!a.empty() && !b.empty()) return a + L"\uFF0C" + b;
    if (!a.empty()) return a;
    return b;
}

std::wstring announce_track_text(metadb_handle_ptr track) {
    downloader_track_info info = get_downloader_track_info(track);
    std::wstring title = trim_text(info.title);
    std::wstring artist = trim_text(info.artist);

    pfc::string8 formatCfg = cfg_announce_track_format.get();
    std::string format = formatCfg.c_str();
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    std::wstring text;
    if (format == "title") {
        text = title;
    } else if (format == "artist_title") {
        text = join_nonempty(artist, title);
    } else if (format == "filename") {
        text = track_file_name(track, false);
    } else if (format == "filename_no_ext") {
        text = track_file_name(track, true);
    } else {
        text = join_nonempty(title, artist);
    }

    if (trim_text(text).empty()) {
        text = track_file_name(track, true);
    }
    if (trim_text(text).empty()) {
        text = fallback_track_path_text(track);
    }
    return trim_text(text);
}

int announce_track_delay_ms() {
    int delay = static_cast<int>(cfg_announce_track_delay_ms.get());
    if (delay < 0) delay = 0;
    if (delay > 10000) delay = 10000;
    return delay;
}

bool queue_or_speak_track_announcement(metadb_handle_ptr track) {
    g_pending_track_announce_text.clear();
    g_pending_track_announce_due_tick = 0;
    if (!cfg_announce_track_on_change.get() || track.is_empty()) return false;

    std::wstring text = announce_track_text(track);
    if (text.empty()) return false;

    int delay = announce_track_delay_ms();
    if (delay <= 0) {
        speech_queue_track_announcement(text.c_str(), true);
    } else {
        g_pending_track_announce_text = text;
        g_pending_track_announce_due_tick = GetTickCount64() + static_cast<ULONGLONG>(delay);
    }
    return true;
}

void cancel_pending_track_announcement() {
    g_pending_track_announce_text.clear();
    g_pending_track_announce_due_tick = 0;
}

void process_pending_track_announcement() {
    if (g_pending_track_announce_text.empty() || g_pending_track_announce_due_tick == 0) return;
    if (GetTickCount64() < g_pending_track_announce_due_tick) return;
    std::wstring text = g_pending_track_announce_text;
    cancel_pending_track_announcement();
    speech_queue_track_announcement(text.c_str(), true);
}

std::wstring lrc_downloader_error_detail(const std::string& output) {
    const std::wstring text = utf8_to_wide(output.c_str());
    std::wstring fallback;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find_first_of(L"\r\n", start);
        std::wstring line = trim_text(text.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty()) {
            fallback = line;
            if (line.rfind(L"ERROR:", 0) == 0) {
                if (line.size() > 512) line.resize(512);
                return line;
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start < text.size() && text[end] == L'\r' && text[start] == L'\n') ++start;
    }
    if (fallback.size() > 512) fallback.resize(512);
    return fallback;
}

void mark_lrc_download_transient_failure(const std::wstring& key,
    const std::wstring& reason, speaklyrics_process_status processStatus,
    DWORD exitCode, DWORD errorCode) {
    const uint64_t delay = g_lrc_download_state.mark_transient_failure(
        key, GetTickCount64(), reason, static_cast<int>(processStatus),
        exitCode, errorCode);
    speaklyrics_log_warning(
        L"自动下载：临时失败，原因=%s，状态=%s，退出码=%lu，错误码=%lu，下次重试=%llu秒后。",
        reason.c_str(), speaklyrics_process_status_name(processStatus),
        exitCode, errorCode, static_cast<unsigned long long>(delay / 1000));
}

void mark_lrc_download_preflight_failure(const std::wstring& key,
    const std::wstring& reason) {
    const uint64_t delay = g_lrc_download_state.mark_transient_failure(
        key, GetTickCount64(), reason);
    speaklyrics_log_warning(
        L"自动下载：临时失败，原因=%s，下次重试=%llu秒后。",
        reason.c_str(), static_cast<unsigned long long>(delay / 1000));
}

void mark_lrc_download_configuration_unavailable(const std::wstring& key,
    const std::wstring& reason, uint64_t retryDelayMs =
        lrc_download_retry_policy::no_retry_tick, DWORD exitCode = 0) {
    g_lrc_download_state.mark_configuration_unavailable(
        key, GetTickCount64(), reason, retryDelayMs, exitCode);
    if (retryDelayMs == lrc_download_retry_policy::no_retry_tick) {
        speaklyrics_log_warning(
            L"自动下载：配置不可用，原因=%s；等待歌曲信息或歌词设置变化后重试。",
            reason.c_str());
    } else {
        speaklyrics_log_warning(
            L"自动下载：配置不可用，原因=%s；%llu秒后重新检查。",
            reason.c_str(), static_cast<unsigned long long>(retryDelayMs / 1000));
    }
}

bool lrc_download_in_progress_for(const std::wstring& key) {
    return g_lrc_downloader_task && g_lrc_download_state.in_progress_for(key);
}



void maybe_start_lrc_downloader(metadb_handle_ptr track) {

    if (track.is_empty()) return;



    std::wstring key = track_key(track);

    if (key.empty()) return;

    const ULONGLONG now = GetTickCount64();
    if (!g_lrc_download_state.can_attempt(key, now)) return;



    std::wstring sources = cfg_path_wide(cfg_lyric_sources);

    if (sources.empty()) {
        mark_lrc_download_configuration_unavailable(key, L"没有启用歌词下载来源");
        return;
    }



    std::wstring permanentFolder = cfg_path_wide(cfg_lrc_folder);
    bool temporaryDownload = trim_text(permanentFolder).empty();
    std::wstring outputFolder = temporaryDownload ? cfg_path_wide(cfg_temp_lrc_folder) : permanentFolder;

    if (outputFolder.empty()) {
        mark_lrc_download_configuration_unavailable(key, L"没有设置可用的歌词输出目录");
        return;
    }



    std::error_code ec;

    fs::create_directories(outputFolder, ec);

    if (ec) {
        log_filesystem_error_once(L"自动下载输出目录", L"创建目录",
            fs::path(outputFolder), ec);
    }

    if (ec || !safe_filesystem_is_directory(
        fs::path(outputFolder), L"自动下载输出目录")) {

        FB2K_console_formatter() << "foo_speaklyrics: lrc download output folder is not available: " << pfc::stringcvt::string_utf8_from_wide(outputFolder.c_str()).get_ptr();
        mark_lrc_download_preflight_failure(
            key, L"歌词输出目录暂时不可用：" + outputFolder);

        return;

    }



    fs::path exePath = fs::path(current_dll_dir()) / L"downloader" / L"LrcDownloader.exe";

    if (!safe_filesystem_exists(exePath, L"自动歌词下载器")) {

        FB2K_console_formatter() << "foo_speaklyrics: downloader not found: " << pfc::stringcvt::string_utf8_from_wide(exePath.c_str()).get_ptr();
        mark_lrc_download_configuration_unavailable(
            key, L"找不到歌词下载器：" + exePath.wstring(),
            lrc_download_retry_policy::missing_downloader_delay_ms);

        return;

    }



    downloader_track_info info = get_downloader_track_info(track);

    if (info.title.empty()) {

        FB2K_console_formatter() << "foo_speaklyrics: downloader skipped because track title is empty";
        mark_lrc_download_configuration_unavailable(key, L"当前歌曲标题为空");

        return;

    }

    if (info.metadata_corrected) {
        speaklyrics_log_info(
            L"自动下载：已纠正错误标签，原标题：%s，原艺术家：%s，改用标题：%s，艺术家：%s。",
            info.original_title.c_str(), info.original_artist.c_str(), info.title.c_str(), info.artist.c_str());
    }



    std::wstring manifestPath = temporaryDownload ? temp_lrc_manifest_path() : L"";

    std::wstring command = command_line_quote(exePath.wstring()) +

        L" --title " + command_line_quote(info.title) +

        L" --artist " + command_line_quote(info.artist) +

        L" --album " + command_line_quote(info.album) +

        L" --duration " + std::to_wstring(info.duration_seconds) +

        L" --sources " + command_line_quote(sources) +

        L" --out " + command_line_quote(outputFolder);

    if (!manifestPath.empty()) command += L" --manifest " + command_line_quote(manifestPath);



    auto task = speaklyrics_start_background_task(L"automatic lyric download");
    if (!task) {
        speaklyrics_log_warning(L"Background task skipped during shutdown.");
        mark_lrc_download_preflight_failure(key, L"无法启动歌词下载后台任务");
        return;
    }
    const uint32_t attempt = g_lrc_download_state.begin_attempt(key);
    g_lrc_downloader_task = task;
    FB2K_console_formatter() << "foo_speaklyrics: started lrc downloader for " << pfc::stringcvt::string_utf8_from_wide(info.title.c_str()).get_ptr();
    speaklyrics_log_info(
        L"自动下载：第%u次尝试开始，标题：%s，艺术家：%s，来源：%s。",
        attempt, info.title.c_str(), info.artist.c_str(), sources.c_str());
    const uint64_t session = g_track_session_id;
    const bool backgroundStarted = speaklyrics_run_background_task(task,
        [task, command, exePath, key, temporaryDownload, session](speaklyrics_background_task& background) {
        const speaklyrics_process_result process = run_process_capture_stdout(
            command, exePath.parent_path(), background.aborter(), 60000, 1024 * 1024);
        std::wstring downloadedPath;
        std::wstring selectedTitle;
        std::wstring selectedArtist;
        if (process.status == speaklyrics_process_status::completed && process.exit_code == 0) {
            parse_candidate_downloader_output(process.output, downloadedPath, selectedTitle, selectedArtist);
        }

        const speaklyrics_process_status status = process.status;
        const DWORD processExitCode = process.exit_code;
        const DWORD processErrorCode = process.error_code;
        const std::wstring processDetail = lrc_downloader_error_detail(process.output);
        background.post_to_main_thread([task, key, temporaryDownload, session, status,
            processExitCode, processErrorCode, processDetail, downloadedPath,
            selectedTitle, selectedArtist]() {
            if (g_lrc_downloader_task.get() == task.get()) {
                g_lrc_downloader_task.reset();
            }
            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != key || g_track_session_id != session) return;
            if (!g_lrc_download_state.in_progress_for(key)) return;

            if (status != speaklyrics_process_status::completed) {
                std::wstring reason = L"下载器进程未正常完成";
                if (!processDetail.empty()) reason += L"：" + processDetail;
                mark_lrc_download_transient_failure(
                    key, reason, status, processExitCode, processErrorCode);
                return;
            }
            if (processExitCode == 1) {
                const uint64_t cooldown = g_lrc_download_state.mark_not_found(
                    key, GetTickCount64(), L"没有匹配的同步歌词", processExitCode);
                speaklyrics_log_warning(
                    L"自动下载：没有匹配结果，进入%llu秒冷却；本地歌词扫描仍会继续。",
                    static_cast<unsigned long long>(cooldown / 1000));
                return;
            }
            if (processExitCode == 2) {
                std::wstring reason = L"下载器参数错误";
                if (!processDetail.empty()) reason += L"：" + processDetail;
                mark_lrc_download_configuration_unavailable(
                    key, reason, lrc_download_retry_policy::no_retry_tick,
                    processExitCode);
                return;
            }
            if (processExitCode != 0) {
                std::wstring reason = L"下载器执行错误或歌词文件提交失败";
                if (!processDetail.empty()) reason += L"：" + processDetail;
                mark_lrc_download_transient_failure(
                    key, reason, status, processExitCode, processErrorCode);
                return;
            }
            if (downloadedPath.empty() || !safe_filesystem_exists(
                fs::path(downloadedPath), L"自动下载结果")) {
                mark_lrc_download_transient_failure(
                    key, L"下载器没有返回可验证的最终 LRC 文件",
                    status, processExitCode, processErrorCode);
                return;
            }

            lrc_document downloadedDocument;
            pfc::string8 loadError;
            if (!downloadedDocument.load(downloadedPath, loadError)) {
                speaklyrics_log_error(L"歌词加载：下载的 LRC 解析失败：%s，文件：%s。",
                    pfc::stringcvt::string_wide_from_utf8(loadError.get_ptr()).get_ptr(), downloadedPath.c_str());
                std::wstring reason = L"下载的 LRC 解析失败：";
                reason += pfc::stringcvt::string_wide_from_utf8(loadError.get_ptr()).get_ptr();
                mark_lrc_download_transient_failure(
                    key, reason, status, processExitCode, processErrorCode);
                return;
            }

            const uint32_t successfulAttempt = g_lrc_download_state.attempt_count;
            speech_invalidate_pending(speech_invalidation_reason::lyrics_reload);
            process_speech_task_results();
            const loaded_lrc_snapshot previous = capture_loaded_lrc_snapshot();
            schedule_current_temp_lrc_delete();
            reset_last_spoken(L"自动下载完成后直接加载歌词");
            g_doc = std::move(downloadedDocument);
            g_current_lrc = downloadedPath;
            g_current_lrc_temporary = temporaryDownload;
            g_loaded_lrc_track_key = track_key(currentTrack);
            mark_document_loaded(L"自动下载结果");
            restore_last_spoken_if_same_lyrics(previous, currentTrack, L"自动下载结果");
            if (g_current_lrc_temporary) cancel_pending_temp_lrc_delete(g_current_lrc);
            refresh_lyrics_jump_window();

            g_lrc_download_state.mark_succeeded(key);
            speaklyrics_log_info(L"自动下载：原子写入及歌词加载已验证，最终文件：%s。",
                downloadedPath.c_str());
            if (successfulAttempt > 1) {
                speaklyrics_log_info(L"自动下载：网络或下载环境恢复后，第%u次尝试成功。",
                    successfulAttempt);
            }

            FB2K_console_formatter() << "foo_speaklyrics: loaded downloaded lrc "
                << pfc::stringcvt::string_utf8_from_wide(downloadedPath.c_str()).get_ptr();
            speaklyrics_log_info(L"歌词加载：已直接加载下载的 LRC：%s，标题：%s，艺术家：%s。",
                downloadedPath.c_str(), selectedTitle.c_str(), selectedArtist.c_str());
            maybe_prefetch_same_title_candidates(currentTrack);
        });
    });
    if (!backgroundStarted) {
        if (g_lrc_downloader_task.get() == task.get()) {
            g_lrc_downloader_task.reset();
        }
        mark_lrc_download_transient_failure(
            key, L"歌词下载后台任务调度失败",
            speaklyrics_process_status::failed_to_start, 3, ERROR_SUCCESS);
    }

}





std::wstring normalize_match_text(const std::wstring& text) {

    std::wstring out;

    out.reserve(text.size());

    for (wchar_t ch : text) {

        if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) {

            out.push_back(static_cast<wchar_t>(towlower(ch)));

        } else if (ch > 127) {

            out.push_back(ch);

        }

    }

    return out;

}



bool contains_match_text(const std::wstring& haystack, const std::wstring& needle) {

    return needle.size() >= 2 && haystack.find(needle) != std::wstring::npos;

}



std::optional<std::wstring> find_lrc_in_folder_impl(const std::wstring& folder,
    metadb_handle_ptr track, const std::optional<std::wstring>& trackPath,
    bool allowFuzzyMatch, const wchar_t* source) {

    if (folder.empty()) return std::nullopt;

    const fs::path folderPath(folder);

    if (!safe_filesystem_is_directory(folderPath, source)) return std::nullopt;



    fs::path base;

    if (trackPath) {

        base = fs::path(*trackPath);

        fs::path candidate = folderPath / (base.stem().wstring() + L".lrc");

        if (safe_filesystem_exists(candidate, source)) return candidate.wstring();

    }



    std::wstring artistText;

    std::wstring titleText;

    file_info_impl info;

    if (track->get_info(info)) {

        const char* artist = info.meta_get("artist", 0);

        const char* title = info.meta_get("title", 0);

        if (artist && *artist) artistText = utf8_to_wide(artist);

        if (title && *title) titleText = utf8_to_wide(title);

        if (!artistText.empty() && !titleText.empty()) {

            std::wstring name = artistText + L" - " + titleText + L".lrc";

            fs::path candidate = folderPath / name;

            if (safe_filesystem_exists(candidate, source)) return candidate.wstring();

            name = titleText + L" - " + artistText + L".lrc";

            candidate = folderPath / name;

            if (safe_filesystem_exists(candidate, source)) return candidate.wstring();

        }

    }



    if (!allowFuzzyMatch) return std::nullopt;

    const std::wstring normalizedStem = trackPath ? normalize_match_text(base.stem().wstring()) : std::wstring();

    const std::wstring normalizedArtist = normalize_match_text(artistText);

    const std::wstring normalizedTitle = normalize_match_text(titleText);



    std::optional<std::wstring> best;

    int bestScore = 0;

    size_t bestNameLength = static_cast<size_t>(-1);

    fs::directory_iterator current;

    if (!safe_filesystem_open_directory(folderPath, source, current)) {

        return std::nullopt;

    }

    const fs::directory_iterator end;

    while (current != end) {

        fs::path path = current->path();

        if (safe_filesystem_is_regular_file(path, source) &&
            _wcsicmp(path.extension().c_str(), L".lrc") == 0) {
            std::wstring normalizedName = normalize_match_text(path.stem().wstring());
            int score = 0;

            if (!normalizedArtist.empty() && !normalizedTitle.empty()) {
                // When an artist is known, never reuse another artist's same-title
                // lyric from a shared folder. This keeps local matching consistent
                // with downloader matching: title + artist first, title-only only
                // when the artist is genuinely unavailable.
                if (contains_match_text(normalizedName, normalizedArtist) &&
                    contains_match_text(normalizedName, normalizedTitle)) {
                    score = 4;
                }
            } else if (!normalizedTitle.empty() &&
                contains_match_text(normalizedName, normalizedTitle)) {
                score = 3;
            } else if (!normalizedStem.empty() &&
                contains_match_text(normalizedName, normalizedStem)) {
                score = 2;
            } else if (!normalizedName.empty() &&
                contains_match_text(normalizedStem, normalizedName)) {
                score = 1;
            }

            size_t nameLength = normalizedName.size();
            if (score > bestScore ||
                (score == bestScore && score > 0 && nameLength < bestNameLength)) {
                best = path.wstring();
                bestScore = score;
                bestNameLength = nameLength;
            }
        }

        if (!safe_filesystem_increment_directory(current, folderPath, source)) break;

    }



    return best;

}

std::optional<std::wstring> find_lrc_in_folder(const std::wstring& folder,
    metadb_handle_ptr track, const std::optional<std::wstring>& trackPath,
    bool allowFuzzyMatch, const wchar_t* source) noexcept {
    try {
        return find_lrc_in_folder_impl(
            folder, track, trackPath, allowFuzzyMatch, source);
    } catch (const fs::filesystem_error& exception) {
        log_filesystem_boundary_failure(source, folder.c_str(), exception.code());
    } catch (const std::bad_alloc&) {
        log_filesystem_boundary_failure(source, folder.c_str(),
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::exception&) {
        log_filesystem_boundary_failure(source, folder.c_str(),
            std::make_error_code(std::errc::io_error));
    } catch (...) {
        log_filesystem_boundary_failure(source, folder.c_str(),
            std::make_error_code(std::errc::io_error));
    }
    return std::nullopt;
}

struct embedded_lyric_diagnostics {
    size_t physical_lines = 0;
    size_t nonempty_lines = 0;
    size_t parsed_lines = 0;
    size_t duplicate_timestamps = 0;
    size_t dense_intervals = 0;
    size_t bom_lines = 0;
    size_t whitespace_before_timestamp = 0;
    size_t angle_timestamp_lines = 0;
    bool literal_line_break_marker = false;
    int minimum_positive_interval_ms = -1;
};

embedded_lyric_diagnostics analyze_embedded_lyrics(const std::wstring& text, const lrc_document& document) {
    embedded_lyric_diagnostics diagnostics;
    diagnostics.parsed_lines = document.count();
    diagnostics.literal_line_break_marker = text.find(L"\\n") != std::wstring::npos ||
        text.find(L"<br") != std::wstring::npos || text.find(L"<BR") != std::wstring::npos;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        std::wstring line = end == std::wstring::npos ? text.substr(start) : text.substr(start, end - start);
        ++diagnostics.physical_lines;

        size_t first = 0;
        while (first < line.size() && iswspace(line[first])) ++first;
        if (first < line.size()) {
            ++diagnostics.nonempty_lines;
            if (line[first] == 0xfeff) {
                ++diagnostics.bom_lines;
                ++first;
                while (first < line.size() && iswspace(line[first])) ++first;
            }
            if (first > 0 && first < line.size() && line[first] == L'[') {
                ++diagnostics.whitespace_before_timestamp;
            }
            if (first < line.size() && line[first] == L'<') ++diagnostics.angle_timestamp_lines;
        }

        if (end == std::wstring::npos) break;
        start = end + 1;
        if (start < text.size() && text[start - 1] == L'\r' && text[start] == L'\n') ++start;
    }

    int previousTime = -1;
    for (size_t index = 0; index < document.count(); ++index) {
        const lrc_line* line = document.get(index);
        if (!line) continue;
        if (previousTime >= 0) {
            const int interval = line->time_ms - previousTime;
            if (interval == 0) {
                ++diagnostics.duplicate_timestamps;
            } else if (interval > 0) {
                if (interval < 1000) ++diagnostics.dense_intervals;
                if (diagnostics.minimum_positive_interval_ms < 0 || interval < diagnostics.minimum_positive_interval_ms) {
                    diagnostics.minimum_positive_interval_ms = interval;
                }
            }
        }
        previousTime = line->time_ms;
    }
    return diagnostics;
}

std::optional<std::wstring> find_embedded_lrc(metadb_handle_ptr track) {
    if (track.is_empty()) return std::nullopt;

    file_info_impl info;
    if (!track->get_info(info)) return std::nullopt;

    const t_size valueCount = info.meta_get_count_by_name("LYRICS");
    if (valueCount == 0) return std::nullopt;

    std::vector<std::wstring> parsedTexts(static_cast<size_t>(valueCount));
    std::vector<size_t> parsedLineCounts(static_cast<size_t>(valueCount), 0);
    size_t parseableValueCount = 0;

    speaklyrics_log_info(L"标签歌词诊断：歌曲会话=%llu，检测到 LYRICS 值数量=%llu。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(valueCount));

    for (t_size index = 0; index < valueCount; ++index) {
        const char* value = info.meta_get("LYRICS", index);
        if (!value || !*value) continue;

        std::wstring text = utf8_to_wide(value);
        lrc_document candidate;
        pfc::string8 error;
        const bool parsed = candidate.load_text(text, L"<LYRICS>", error);
        const embedded_lyric_diagnostics diagnostics = analyze_embedded_lyrics(text, candidate);
        speaklyrics_log_info(
            L"标签歌词诊断：值=%llu/%llu，字符=%llu，物理行=%llu，非空行=%llu，有效时间行=%llu，重复时间戳=%llu，小于1秒间隔=%llu，最短正间隔=%d毫秒，BOM行=%llu，时间戳前空白行=%llu，尖括号开头行=%llu，字面换行标记=%s。",
            static_cast<unsigned long long>(index + 1), static_cast<unsigned long long>(valueCount),
            static_cast<unsigned long long>(text.size()),
            static_cast<unsigned long long>(diagnostics.physical_lines),
            static_cast<unsigned long long>(diagnostics.nonempty_lines),
            static_cast<unsigned long long>(diagnostics.parsed_lines),
            static_cast<unsigned long long>(diagnostics.duplicate_timestamps),
            static_cast<unsigned long long>(diagnostics.dense_intervals),
            diagnostics.minimum_positive_interval_ms,
            static_cast<unsigned long long>(diagnostics.bom_lines),
            static_cast<unsigned long long>(diagnostics.whitespace_before_timestamp),
            static_cast<unsigned long long>(diagnostics.angle_timestamp_lines),
            diagnostics.literal_line_break_marker ? L"是" : L"否");

        if (diagnostics.duplicate_timestamps > 0) {
            speaklyrics_log_warning(L"标签歌词诊断：值=%llu 包含 %llu 个重复时间戳，当前朗读逻辑可能只选择同时间戳的最后一行。",
                static_cast<unsigned long long>(index + 1),
                static_cast<unsigned long long>(diagnostics.duplicate_timestamps));
        }
        if (diagnostics.dense_intervals > 0) {
            speaklyrics_log_warning(L"标签歌词诊断：值=%llu 包含 %llu 个小于1秒的歌词间隔，每秒播放回调可能跨过部分歌词。",
                static_cast<unsigned long long>(index + 1),
                static_cast<unsigned long long>(diagnostics.dense_intervals));
        }
        if (diagnostics.bom_lines > 0 || diagnostics.whitespace_before_timestamp > 0 ||
            diagnostics.angle_timestamp_lines > 0 || diagnostics.literal_line_break_marker) {
            speaklyrics_log_warning(
                L"标签歌词诊断：值=%llu 检测到可能影响解析的文本格式，请结合 BOM、行首空白、尖括号和字面换行统计排查漏读。",
                static_cast<unsigned long long>(index + 1));
        }

        if (parsed) {
            ++parseableValueCount;
            const size_t valueIndex = static_cast<size_t>(index);
            parsedLineCounts[valueIndex] = candidate.count();
            parsedTexts[valueIndex] = std::move(text);
        } else {
            speaklyrics_log_warning(
                L"标签歌词：LYRICS 的第 %llu 个值不包含可用时间戳，解析信息=%s。",
                static_cast<unsigned long long>(index + 1),
                pfc::stringcvt::string_wide_from_utf8(error.get_ptr()).get_ptr());
        }
    }

    const auto selectedValueIndex =
        speaklyrics_embedded_lyrics::select_best_value_index(parsedLineCounts);
    if (!selectedValueIndex) return std::nullopt;

    const size_t selectedIndex = *selectedValueIndex;
    const size_t selectedLineCount = parsedLineCounts[selectedIndex];
    size_t equalBestValueCount = 0;
    for (const size_t lineCount : parsedLineCounts) {
        if (lineCount == selectedLineCount) ++equalBestValueCount;
    }

    speaklyrics_log_info(L"标签歌词诊断：当前选择第 %llu 个 LYRICS 值，有效歌词=%llu行，可解析值总数=%llu。",
        static_cast<unsigned long long>(selectedIndex + 1),
        static_cast<unsigned long long>(selectedLineCount),
        static_cast<unsigned long long>(parseableValueCount));
    if (parseableValueCount > 1) {
        speaklyrics_log_info(
            L"标签歌词诊断：存在 %llu 个可解析的 LYRICS 值，已选择有效时间歌词行最多的第 %llu 个值；多个值不会自动合并。",
            static_cast<unsigned long long>(parseableValueCount),
            static_cast<unsigned long long>(selectedIndex + 1));
    }
    if (equalBestValueCount > 1) {
        speaklyrics_log_info(
            L"标签歌词诊断：有 %llu 个值同为最多的 %llu 行，按标签顺序选择最前面的第 %llu 个值。",
            static_cast<unsigned long long>(equalBestValueCount),
            static_cast<unsigned long long>(selectedLineCount),
            static_cast<unsigned long long>(selectedIndex + 1));
    }
    return std::move(parsedTexts[selectedIndex]);
}



std::optional<lrc_match> find_lrc_for_track_impl(metadb_handle_ptr track) {

    std::wstring manual = cfg_path_wide(cfg_lrc_file);

    if (!manual.empty() && !g_manual_lrc_track_key.empty() &&
        g_manual_lrc_track_key == track_key(track) &&
        safe_filesystem_exists(fs::path(manual), L"手动加载 LRC")) {

        return lrc_match{ manual, false, std::wstring() };

    }



    auto trackPath = local_track_path(track);

    std::optional<std::wstring> trackFolder;

    if (trackPath) {

        trackFolder = fs::path(*trackPath).parent_path().wstring();

        if (auto local = find_lrc_in_folder(*trackFolder, track, trackPath, false,
            L"歌曲同目录精确匹配")) {
            return lrc_match{ *local, false, std::wstring() };
        }

    }



    std::wstring folder = cfg_path_wide(cfg_lrc_folder);

    if (auto normal = find_lrc_in_folder(folder, track, trackPath, false,
        L"正式 LRC 目录精确匹配")) {
        return lrc_match{ *normal, false, std::wstring() };
    }

    if (auto embedded = find_embedded_lrc(track)) {
        return lrc_match{ std::wstring(), false, std::move(*embedded) };
    }

    if (trackFolder) {
        if (auto local = find_lrc_in_folder(*trackFolder, track, trackPath, true,
            L"歌曲同目录模糊匹配")) {
            return lrc_match{ *local, false, std::wstring() };
        }
    }

    if (auto normal = find_lrc_in_folder(folder, track, trackPath, true,
        L"正式 LRC 目录模糊匹配")) {
        return lrc_match{ *normal, false, std::wstring() };
    }



    std::wstring tempFolder = cfg_path_wide(cfg_temp_lrc_folder);

    if (auto temp = find_lrc_in_folder(tempFolder, track, trackPath, true,
        L"临时 LRC 目录")) {
        return lrc_match{ *temp, true, std::wstring() };
    }



    return std::nullopt;

}

std::optional<lrc_match> find_lrc_for_track(metadb_handle_ptr track) noexcept {
    try {
        return find_lrc_for_track_impl(track);
    } catch (const fs::filesystem_error& exception) {
        log_filesystem_boundary_failure(L"歌词来源总入口",
            exception.path1().empty() ? L"" : exception.path1().c_str(),
            exception.code());
    } catch (const std::bad_alloc&) {
        log_filesystem_boundary_failure(L"歌词来源总入口", L"",
            std::make_error_code(std::errc::not_enough_memory));
    } catch (const std::exception&) {
        log_filesystem_boundary_failure(L"歌词来源总入口", L"",
            std::make_error_code(std::errc::io_error));
    } catch (...) {
        log_filesystem_boundary_failure(L"歌词来源总入口", L"",
            std::make_error_code(std::errc::io_error));
    }
    return std::nullopt;
}

}



bool copy_text_to_clipboard(const std::wstring& text) {
    if (text.empty()) return false;
    HWND wnd = core_api::get_main_window();
    if (!OpenClipboard(wnd)) return false;
    EmptyClipboard();

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) {
        CloseClipboard();
        return false;
    }

    void* ptr = GlobalLock(mem);
    if (!ptr) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(mem);

    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }

    CloseClipboard();
    return true;
}

namespace {

static bool same_path_text(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static int temp_lrc_delete_delay_ms() {
    int delay = static_cast<int>(cfg_temp_lrc_delete_delay_ms.get());
    if (delay < 0) delay = 0;
    if (delay > 600000) delay = 600000;
    return delay;
}

bool delete_temp_lrc_path(const std::wstring& path) {

    if (path.empty()) return false;

    std::wstring tempFolder = cfg_path_wide(cfg_temp_lrc_folder);

    if (tempFolder.empty()) return false;



    std::error_code ec;

    fs::path file(path);

    if (!fs::exists(file, ec) || !fs::is_regular_file(file, ec)) return false;



    fs::path parent = fs::weakly_canonical(file.parent_path(), ec);

    if (ec) return false;

    fs::path temp = fs::weakly_canonical(fs::path(tempFolder), ec);

    if (ec) return false;



    if (parent == temp) {

        fs::remove(file, ec);

        if (!ec) {

            FB2K_console_formatter() << "foo_speaklyrics: deleted temporary lrc " << pfc::stringcvt::string_utf8_from_wide(file.c_str()).get_ptr();

            return true;

        }

    }

    return false;

}

void cancel_pending_temp_lrc_delete(const std::wstring& path) {
    if (path.empty()) return;
    g_pending_temp_lrc_deletes.erase(
        std::remove_if(g_pending_temp_lrc_deletes.begin(), g_pending_temp_lrc_deletes.end(),
            [&](const pending_temp_lrc_delete& item) { return same_path_text(item.path, path); }),
        g_pending_temp_lrc_deletes.end());
}

void process_pending_temp_lrc_deletes() {
    if (g_pending_temp_lrc_deletes.empty()) return;
    const ULONGLONG now = GetTickCount64();
    auto it = g_pending_temp_lrc_deletes.begin();
    while (it != g_pending_temp_lrc_deletes.end()) {
        if (it->due_tick > now) {
            ++it;
            continue;
        }
        if (g_current_lrc_temporary && same_path_text(g_current_lrc, it->path)) {
            it = g_pending_temp_lrc_deletes.erase(it);
            continue;
        }
        delete_temp_lrc_path(it->path);
        it = g_pending_temp_lrc_deletes.erase(it);
    }
}

void schedule_temp_lrc_delete(const std::wstring& path) {
    if (path.empty()) return;
    cancel_pending_temp_lrc_delete(path);
    const int delay = temp_lrc_delete_delay_ms();
    if (delay <= 0) {
        delete_temp_lrc_path(path);
        return;
    }
    g_pending_temp_lrc_deletes.push_back({ path, GetTickCount64() + static_cast<ULONGLONG>(delay) });
}

void schedule_current_temp_lrc_delete() {

    if (!g_current_lrc_temporary || g_current_lrc.empty()) return;

    schedule_temp_lrc_delete(g_current_lrc);

}

void delete_current_temp_lrc() {

    if (!g_current_lrc_temporary || g_current_lrc.empty()) return;

    delete_temp_lrc_path(g_current_lrc);

}



void log_track_diagnostic_summary(const wchar_t* reason) {
    if (g_track_session_id == 0 || g_track_summary_logged) return;
    speaklyrics_log_info(
        L"歌词诊断汇总：歌曲会话=%llu，结束原因=%s，计划=%llu，入队=%llu，已提交接口=%llu，失败=%llu，任务过期=%llu，取消=%llu，入队拒绝=%llu，疑似重复=%llu，时间轴跨过=%llu，超过有效时间=%llu，延迟回调=%llu，切歌播报阻塞=%llu。",
        static_cast<unsigned long long>(g_track_session_id), reason ? reason : L"未知",
        static_cast<unsigned long long>(g_track_diagnostics.planned),
        static_cast<unsigned long long>(g_track_diagnostics.accepted),
        static_cast<unsigned long long>(g_track_diagnostics.dispatched),
        static_cast<unsigned long long>(g_track_diagnostics.failed),
        static_cast<unsigned long long>(g_track_diagnostics.task_expired),
        static_cast<unsigned long long>(g_track_diagnostics.canceled),
        static_cast<unsigned long long>(g_track_diagnostics.rejected),
        static_cast<unsigned long long>(g_track_diagnostics.suspected_duplicates),
        static_cast<unsigned long long>(g_track_diagnostics.crossed_lines),
        static_cast<unsigned long long>(g_track_diagnostics.expired_lines),
        static_cast<unsigned long long>(g_track_diagnostics.delayed_callbacks),
        static_cast<unsigned long long>(g_track_diagnostics.announcement_blocks));
    g_track_summary_logged = true;
}

void reset_track_diagnostic_state() {
    g_track_diagnostics = track_diagnostic_counters{};
    g_filesystem_error_log_keys.clear();
    g_last_playback_callback_time = -1.0;
    g_pending_announcement_skip_logged = false;
    g_last_skip_document_generation = 0;
    g_last_skip_line_id = 0;
    g_last_skip_reason = 0;
    g_track_summary_logged = false;
}

uint64_t fnv1a_mix_u64(uint64_t hash, uint64_t value) {
    constexpr uint64_t prime = 1099511628211ULL;
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xff;
        hash *= prime;
    }
    return hash;
}

uint64_t current_lyric_document_hash() {
    uint64_t hash = 1469598103934665603ULL;
    hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(g_doc.count()));
    for (size_t index = 0; index < g_doc.count(); ++index) {
        const lrc_line* line = g_doc.get(index);
        if (!line) continue;
        hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(line->time_ms));
        hash = fnv1a_mix_u64(hash, speaklyrics_log_text_hash(line->text.c_str()));
        hash = fnv1a_mix_u64(hash, static_cast<uint64_t>(line->text.size()));
    }
    return hash;
}

loaded_lrc_snapshot capture_loaded_lrc_snapshot() {
    loaded_lrc_snapshot snapshot;
    if (g_doc.empty() || g_loaded_lrc_track_key.empty()) return snapshot;
    snapshot.valid = true;
    snapshot.track_key = g_loaded_lrc_track_key;
    snapshot.document_hash = current_lyric_document_hash();
    snapshot.dispatched_line_ids.reserve(g_dispatched_line_ids.size());
    for (const uint64_t lineId : g_dispatched_line_ids) {
        snapshot.dispatched_line_ids.push_back(lineId);
    }
    std::sort(snapshot.dispatched_line_ids.begin(), snapshot.dispatched_line_ids.end());
    snapshot.last_dispatched_line_id = g_last_dispatched_line_id;
    snapshot.last_dispatched_task_id = g_last_dispatched_task_id;
    return snapshot;
}

void restore_last_spoken_if_same_lyrics(const loaded_lrc_snapshot& snapshot, metadb_handle_ptr track,
    const wchar_t* source) {
    if (!snapshot.valid || g_doc.empty()) return;
    const std::wstring currentKey = track_key(track);
    if (currentKey.empty() || currentKey != snapshot.track_key) return;
    const uint64_t newHash = current_lyric_document_hash();
    if (newHash != snapshot.document_hash) return;

    g_dispatched_line_ids.clear();
    for (const uint64_t lineId : snapshot.dispatched_line_ids) {
        if (lineId != 0) g_dispatched_line_ids.insert(lineId);
    }
    g_last_dispatched_line_id = snapshot.last_dispatched_line_id;
    g_last_dispatched_task_id = snapshot.last_dispatched_task_id;
    g_last_scheduled_line_id = g_last_dispatched_line_id;
    speaklyrics_log_info(
        L"歌词重载：歌曲会话=%llu，歌词文档=%llu，来源=%s，检测到相同歌曲和相同歌词，保留已提交歌词行=%llu行，最后歌词行ID=%llu。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_document_generation),
        source ? source : L"未知",
        static_cast<unsigned long long>(g_dispatched_line_ids.size()),
        static_cast<unsigned long long>(g_last_dispatched_line_id));
}

void prune_recent_lyric_submissions(ULONGLONG now) {
    g_recent_lyric_submissions.erase(
        std::remove_if(g_recent_lyric_submissions.begin(), g_recent_lyric_submissions.end(),
            [now](const lyric_submission_record& record) {
                return now < record.submitted_at || now - record.submitted_at > 10000;
            }),
        g_recent_lyric_submissions.end());
    constexpr size_t kMaximumRecentSubmissions = 128;
    if (g_recent_lyric_submissions.size() > kMaximumRecentSubmissions) {
        const size_t removeCount = g_recent_lyric_submissions.size() - kMaximumRecentSubmissions;
        g_recent_lyric_submissions.erase(
            g_recent_lyric_submissions.begin(), g_recent_lyric_submissions.begin() + removeCount);
    }
}

void remove_recent_lyric_submission(uint64_t taskId) {
    if (taskId == 0) return;
    g_recent_lyric_submissions.erase(
        std::remove_if(g_recent_lyric_submissions.begin(), g_recent_lyric_submissions.end(),
            [taskId](const lyric_submission_record& record) {
                return record.speech_task_id == taskId;
            }),
        g_recent_lyric_submissions.end());
}

void reset_last_spoken(const wchar_t* reason) {
    speaklyrics_log_info(
        L"朗读状态重置：歌曲会话=%llu，歌词文档=%llu，原计划歌词行ID=%llu，原提交歌词行ID=%llu，等待任务=%llu，原因=%s。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_document_generation),
        static_cast<unsigned long long>(g_last_scheduled_line_id),
        static_cast<unsigned long long>(g_last_dispatched_line_id),
        static_cast<unsigned long long>(g_pending_lyric_submissions.size()),
        reason ? reason : L"未知");
    g_last_scheduled_line_id = 0;
    g_last_dispatched_line_id = 0;
    g_last_dispatched_task_id = 0;
    g_scheduler_scan_trigger_ms = -1;
    g_pending_lyric_submissions.clear();
    g_scheduled_line_ids.clear();
    g_dispatched_line_ids.clear();
    g_skipped_line_ids.clear();
    g_retry_line_ids.clear();
    g_last_lyric_submission = lyric_submission_record{};
    g_last_skip_document_generation = 0;
    g_last_skip_line_id = 0;
    g_last_skip_reason = 0;
}

void advance_lyric_position_epoch(const wchar_t* reason) {
    ++g_lyric_position_epoch;
    if (g_lyric_position_epoch == 0) g_lyric_position_epoch = 1;
    speaklyrics_log_info(L"歌词播放位置世代递增：歌曲会话=%llu，位置世代=%llu，原因=%s。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_lyric_position_epoch),
        reason ? reason : L"未知");
}

void recalculate_last_scheduled() {
    uint64_t scheduled = g_last_dispatched_line_id;
    uint64_t newestTaskId = g_last_dispatched_task_id;
    const uint64_t playbackGeneration = speech_current_playback_generation();
    for (const auto& pending : g_pending_lyric_submissions) {
        if (pending.track_session != g_track_session_id ||
            pending.playback_generation != playbackGeneration ||
            pending.position_epoch != g_lyric_position_epoch) continue;
        if (pending.speech_task_id >= newestTaskId) {
            newestTaskId = pending.speech_task_id;
            scheduled = pending.line_ids.empty() ? pending.line_id : pending.line_ids.back();
        }
    }
    g_last_scheduled_line_id = scheduled;
}

void process_speech_task_results() {
    std::vector<speech_task_result> results = speech_drain_task_results();
    if (results.empty()) return;

    const uint64_t activePlaybackGeneration = speech_current_playback_generation();
    for (const auto& result : results) {
        auto pending = std::find_if(g_pending_lyric_submissions.begin(),
            g_pending_lyric_submissions.end(), [&result](const lyric_submission_record& record) {
                return record.speech_task_id == result.task.task_id;
            });

        lyric_submission_record record;
        if (pending != g_pending_lyric_submissions.end()) {
            record = *pending;
            g_pending_lyric_submissions.erase(pending);
        } else {
            record.track_session = result.task.track_session;
            record.document_generation = result.task.document_generation;
            record.playback_generation = result.task.playback_generation;
            record.speech_task_id = result.task.task_id;
            record.line_index = result.task.line_index;
            record.line_id = result.task.line_id;
            record.line_ids = result.task.line_ids;
            if (record.line_ids.empty() && result.task.line_id != 0) {
                record.line_ids.push_back(result.task.line_id);
            }
            record.position_epoch = result.task.position_epoch;
            record.lyric_time_ms = result.task.lyric_time_ms;
            record.trigger_time_ms = result.task.trigger_time_ms;
            record.text_hash = result.task.text_hash;
            record.submitted_at = result.task.queued_at;
            record.expires_at = result.task.expires_at;
        }

        const bool belongsToCurrentTrack = record.track_session == g_track_session_id;
        const bool belongsToCurrentPosition =
            belongsToCurrentTrack &&
            record.playback_generation == activePlaybackGeneration &&
            record.position_epoch == g_lyric_position_epoch;
        const bool belongsToCurrentDocument = belongsToCurrentPosition &&
            record.document_generation == g_document_generation;
        const bool belongsToCurrentLineState = belongsToCurrentTrack &&
            record.document_generation == g_document_generation &&
            record.position_epoch == g_lyric_position_epoch;

        std::vector<uint64_t> recordLineIds = record.line_ids;
        if (recordLineIds.empty() && record.line_id != 0) recordLineIds.push_back(record.line_id);

        switch (result.status) {
        case speech_task_result_status::dispatched:
            if (belongsToCurrentTrack) ++g_track_diagnostics.dispatched;
            if (belongsToCurrentLineState) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId == 0) continue;
                    g_scheduled_line_ids.erase(lineId);
                    g_retry_line_ids.erase(lineId);
                }
            }
            if (belongsToCurrentDocument) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId != 0) g_dispatched_line_ids.insert(lineId);
                }
            }
            if (belongsToCurrentDocument && record.speech_task_id >= g_last_dispatched_task_id) {
                g_last_dispatched_line_id = recordLineIds.empty() ? record.line_id : recordLineIds.back();
                g_last_dispatched_task_id = record.speech_task_id;
            }
            break;
        case speech_task_result_status::failed:
            if (belongsToCurrentTrack) ++g_track_diagnostics.failed;
            if (belongsToCurrentLineState) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId != 0) g_scheduled_line_ids.erase(lineId);
                }
            }
            remove_recent_lyric_submission(record.speech_task_id);
            if (belongsToCurrentLineState &&
                (record.expires_at == 0 || GetTickCount64() < record.expires_at)) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId != 0) g_retry_line_ids.insert(lineId);
                }
            }
            break;
        case speech_task_result_status::expired:
            if (belongsToCurrentTrack) ++g_track_diagnostics.task_expired;
            if (belongsToCurrentLineState) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId == 0) continue;
                    g_scheduled_line_ids.erase(lineId);
                    g_retry_line_ids.erase(lineId);
                    g_skipped_line_ids.insert(lineId);
                }
            }
            remove_recent_lyric_submission(record.speech_task_id);
            break;
        case speech_task_result_status::canceled:
            if (belongsToCurrentTrack) ++g_track_diagnostics.canceled;
            if (belongsToCurrentLineState) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId != 0) g_scheduled_line_ids.erase(lineId);
                }
            }
            remove_recent_lyric_submission(record.speech_task_id);
            if (belongsToCurrentLineState &&
                (record.expires_at == 0 || GetTickCount64() < record.expires_at)) {
                for (const uint64_t lineId : recordLineIds) {
                    if (lineId != 0) g_retry_line_ids.insert(lineId);
                }
            }
            break;
        }

        if (result.status != speech_task_result_status::dispatched &&
            g_last_lyric_submission.speech_task_id == record.speech_task_id) {
            g_last_lyric_submission = lyric_submission_record{};
        }

        recalculate_last_scheduled();
        const wchar_t* resultText = L"未知";
        switch (result.status) {
        case speech_task_result_status::dispatched: resultText = L"已提交给语音接口"; break;
        case speech_task_result_status::failed: resultText = L"失败"; break;
        case speech_task_result_status::expired: resultText = L"过期"; break;
        case speech_task_result_status::canceled: resultText = L"取消"; break;
        }
        speaklyrics_log_info(
            L"朗读状态更新：任务=%llu，结果=%s，当前歌曲=%s，当前播放位置=%s，当前歌词文档=%s，位置世代=%llu，文档序号=%d，歌词行ID=%llu，关联歌词行=%llu，歌词时间=%d，触发时间=%d，计划歌词行ID=%llu，已提交歌词行ID=%llu，剩余等待=%llu。",
            static_cast<unsigned long long>(record.speech_task_id),
            resultText, belongsToCurrentTrack ? L"是" : L"否",
            belongsToCurrentPosition ? L"是" : L"否",
            belongsToCurrentDocument ? L"是" : L"否",
            static_cast<unsigned long long>(record.position_epoch),
            record.line_index, static_cast<unsigned long long>(record.line_id),
            static_cast<unsigned long long>(recordLineIds.size()),
            record.lyric_time_ms, record.trigger_time_ms,
            static_cast<unsigned long long>(g_last_scheduled_line_id),
            static_cast<unsigned long long>(g_last_dispatched_line_id),
            static_cast<unsigned long long>(g_pending_lyric_submissions.size()));
    }
}

void mark_document_loaded(const wchar_t* source) {
    ++g_document_generation;
    size_t duplicateTimestamps = 0;
    size_t denseIntervals = 0;
    int minimumPositiveInterval = -1;
    int previousTime = -1;
    for (size_t index = 0; index < g_doc.count(); ++index) {
        const lrc_line* line = g_doc.get(index);
        if (!line) continue;
        if (previousTime >= 0) {
            const int interval = line->time_ms - previousTime;
            if (interval == 0) {
                ++duplicateTimestamps;
            } else if (interval > 0) {
                if (interval < 1000) ++denseIntervals;
                if (minimumPositiveInterval < 0 || interval < minimumPositiveInterval) minimumPositiveInterval = interval;
            }
        }
        previousTime = line->time_ms;
    }

    speaklyrics_log_info(
        L"歌词文档加载：歌曲会话=%llu，歌词文档=%llu，来源=%s，歌词行=%llu，重复时间戳=%llu，小于1秒间隔=%llu，最短正间隔=%d毫秒。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_document_generation),
        source ? source : L"未知",
        static_cast<unsigned long long>(g_doc.count()),
        static_cast<unsigned long long>(duplicateTimestamps),
        static_cast<unsigned long long>(denseIntervals), minimumPositiveInterval);
    if (duplicateTimestamps > 0) {
        speaklyrics_log_info(L"歌词文档诊断：歌词文档=%llu 含有 %llu 个重复时间戳；自动朗读会按原文件顺序合并为同一个语音任务。",
            static_cast<unsigned long long>(g_document_generation),
            static_cast<unsigned long long>(duplicateTimestamps));
    }
    if (denseIntervals > 0) {
        speaklyrics_log_info(L"歌词文档诊断：歌词文档=%llu 含有 %llu 个小于1秒的间隔；播放回调跨过多句时会按顺序补交仍在有效时间内的歌词组。",
            static_cast<unsigned long long>(g_document_generation),
            static_cast<unsigned long long>(denseIntervals));
    }
}

void clear_loaded_lrc(const wchar_t* reason) {

    reset_last_spoken(reason);
    g_doc.clear();

    g_loaded_lrc_track_key.clear();

    g_current_lrc.clear();

    g_current_lrc_temporary = false;

}



void load_for_track(metadb_handle_ptr track, const wchar_t* reason);



int missing_lrc_retry_ms() {

    int ms = static_cast<int>(cfg_missing_lrc_retry_ms.get());

    if (ms > 0 && ms < 100) ms *= 1000;

    if (ms <= 0) return 3000;

    if (ms < 500) return 500;

    if (ms > 600000) return 600000;

    return ms;

}



int lyric_valid_ms() {

    int ms = static_cast<int>(cfg_lyric_valid_ms.get());

    if (ms <= 0) return 3000;

    if (ms > 60000) return 60000;

    return ms;

}



bool retry_load_missing_lrc(double) {

    if (!g_doc.empty() || g_paused) return false;

    const ULONGLONG now = GetTickCount64();
    if (g_last_missing_lrc_scan_tick != 0 &&
        now - g_last_missing_lrc_scan_tick < static_cast<ULONGLONG>(missing_lrc_retry_ms())) {
        return false;
    }
    g_last_missing_lrc_scan_tick = now;



    metadb_handle_ptr track;

    if (!static_api_ptr_t<playback_control>()->get_now_playing(track)) return false;



    load_for_track(track, L"无歌词后台扫描重试");

    return !g_doc.empty();

}



void load_for_track(metadb_handle_ptr track, const wchar_t* reason) {

    process_pending_temp_lrc_deletes();

    const loaded_lrc_snapshot previous = capture_loaded_lrc_snapshot();

    clear_loaded_lrc(reason);

    auto found = find_lrc_for_track(track);

    if (!found) {
        const std::wstring key = track_key(track);
        if (!g_lrc_download_state.is_for(key) ||
            g_lrc_download_state.status == lrc_download_status::idle) {
            speaklyrics_log_warning(L"歌词加载：未找到可用 LRC，准备按设置尝试下载。");
        }
        maybe_start_lrc_downloader(track);

        refresh_lyrics_jump_window();

        return;

    }

    pfc::string8 error;

    if (!found->embedded_text.empty()) {
        if (g_doc.load_text(found->embedded_text, L"<LYRICS>", error)) {
            g_loaded_lrc_track_key = track_key(track);
            mark_document_loaded(L"音频文件内嵌 LYRICS 标签");
            restore_last_spoken_if_same_lyrics(previous, track, L"音频文件内嵌 LYRICS 标签");
            speaklyrics_log_info(
                L"歌词加载：已加载音频文件内嵌 LYRICS 标签，共 %llu 行。",
                static_cast<unsigned long long>(g_doc.count()));
            maybe_prefetch_same_title_candidates(track);
        } else {
            speaklyrics_log_error(
                L"歌词加载：内嵌 LYRICS 标签解析失败：%s。",
                pfc::stringcvt::string_wide_from_utf8(error.get_ptr()).get_ptr());
            maybe_start_lrc_downloader(track);
        }
        refresh_lyrics_jump_window();
        return;
    }

    if (g_doc.load(found->path, error)) {

        g_current_lrc = found->path;

        g_current_lrc_temporary = found->temporary;
        g_loaded_lrc_track_key = track_key(track);
        mark_document_loaded(found->temporary ? L"临时歌词目录 LRC" : L"本地或指定目录 LRC");
        restore_last_spoken_if_same_lyrics(previous, track,
            found->temporary ? L"临时歌词目录 LRC" : L"本地或指定目录 LRC");

        if (g_current_lrc_temporary) cancel_pending_temp_lrc_delete(g_current_lrc);

        FB2K_console_formatter() << "foo_speaklyrics: loaded " << pfc::stringcvt::string_utf8_from_wide(found->path.c_str()).get_ptr();
        speaklyrics_log_info(L"歌词加载：已加载 LRC：%s。", found->path.c_str());
        maybe_prefetch_same_title_candidates(track);

    } else {

        FB2K_console_formatter() << "foo_speaklyrics: " << error;
        speaklyrics_log_error(L"歌词加载：解析失败：%s，文件：%s。", pfc::stringcvt::string_wide_from_utf8(error.get_ptr()).get_ptr(), found->path.c_str());

    }

    refresh_lyrics_jump_window();

}



bool is_cjk_speech_character(wchar_t ch) {
    return (ch >= 0x3400 && ch <= 0x9fff) || (ch >= 0x3040 && ch <= 0x30ff) ||
        (ch >= 0xac00 && ch <= 0xd7af);
}

int estimate_lyric_speech_ms(const std::wstring& text) {
    int cjkCharacters = 0;
    int latinWords = 0;
    int punctuation = 0;
    bool insideLatinWord = false;

    for (wchar_t ch : text) {
        if (is_cjk_speech_character(ch)) {
            ++cjkCharacters;
            insideLatinWord = false;
        } else if (iswalnum(ch)) {
            if (!insideLatinWord) ++latinWords;
            insideLatinWord = true;
        } else {
            insideLatinWord = false;
            if (iswpunct(ch)) ++punctuation;
        }
    }

    // This is intentionally approximate: Tolk does not expose the active
    // screen reader's speech rate or a reliable completion event.
    int estimate = 400 + cjkCharacters * 180 + latinWords * 360 + punctuation * 90;
    return (std::clamp)(estimate, 900, 6000);
}

enum class lyric_submit_status {
    accepted,
    duplicate,
    expired,
    rejected,
};

void log_skipped_lyric_once(int reason, const lyric_schedule_item& item, int playbackMs,
    const wchar_t* reasonText) {
    if (g_last_skip_document_generation == g_document_generation &&
        g_last_skip_line_id == item.line_id && g_last_skip_reason == reason) return;

    g_last_skip_document_generation = g_document_generation;
    g_last_skip_line_id = item.line_id;
    g_last_skip_reason = reason;
    if (reason == 1) ++g_track_diagnostics.expired_lines;

    const std::wstring excerpt = speaklyrics_log_text_excerpt(item.text.c_str());
    speaklyrics_log_warning(
        L"歌词跳过：歌曲会话=%llu，歌词文档=%llu，文档序号=%llu，歌词行ID=%llu，播放时间=%d，歌词时间=%d，触发时间=%d，落后=%d毫秒，原因=%s，文本哈希=%016llX，内容=%s。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_document_generation),
        static_cast<unsigned long long>(item.document_index),
        static_cast<unsigned long long>(item.line_id),
        playbackMs, item.lyric_time_ms, item.trigger_time_ms,
        playbackMs - item.lyric_time_ms,
        reasonText ? reasonText : L"未知",
        static_cast<unsigned long long>(speaklyrics_log_text_hash(item.text.c_str())),
        excerpt.c_str());
}

void log_crossed_lyrics_if_needed(size_t dueCount, size_t groupCount,
    int previousTriggerMs, int playbackMs, const wchar_t* mode) {
    if (dueCount <= 1 && groupCount <= 1) return;
    const size_t crossed = groupCount > 1 ? groupCount - 1 : 0;
    g_track_diagnostics.crossed_lines += static_cast<uint64_t>(crossed);
    speaklyrics_log_warning(
        L"时间轴批量调度：歌曲会话=%llu，歌词文档=%llu，位置世代=%llu，扫描区间起点=%d，播放时间=%d，发现到期=%llu行，共%llu个歌词组，模式=%s；同时间戳多行合并，跨越的歌词组按原始顺序补交。",
        static_cast<unsigned long long>(g_track_session_id),
        static_cast<unsigned long long>(g_document_generation),
        static_cast<unsigned long long>(g_lyric_position_epoch),
        previousTriggerMs, playbackMs,
        static_cast<unsigned long long>(dueCount),
        static_cast<unsigned long long>(groupCount), mode ? mode : L"未知");
}

std::vector<uint64_t> schedule_group_line_ids(
    const std::vector<lyric_schedule_item>& schedule,
    const std::vector<size_t>& group) {
    std::vector<uint64_t> lineIds;
    lineIds.reserve(group.size());
    for (const size_t index : group) {
        if (index >= schedule.size() || schedule[index].line_id == 0) continue;
        lineIds.push_back(schedule[index].line_id);
    }
    return lineIds;
}

std::wstring schedule_group_text(
    const std::vector<lyric_schedule_item>& schedule,
    const std::vector<size_t>& group) {
    std::wstring text;
    for (const size_t index : group) {
        if (index >= schedule.size() || schedule[index].text.empty()) continue;
        if (!text.empty()) text += L"\r\n";
        text += schedule[index].text;
    }
    return text;
}

bool recent_submission_matches(const lyric_schedule_item& item,
    const std::vector<uint64_t>& lineIds, uint64_t textHash,
    ULONGLONG now, lyric_submission_record& matched) {
    prune_recent_lyric_submissions(now);
    for (const auto& record : g_recent_lyric_submissions) {
        if (record.track_session != g_track_session_id ||
            record.position_epoch != g_lyric_position_epoch ||
            record.lyric_time_ms != item.lyric_time_ms ||
            record.text_hash != textHash ||
            now < record.submitted_at || now - record.submitted_at > 10000) continue;

        std::vector<uint64_t> recordLineIds = record.line_ids;
        if (recordLineIds.empty() && record.line_id != 0) recordLineIds.push_back(record.line_id);
        const bool sameStableLines = !lineIds.empty() && recordLineIds == lineIds;
        const bool documentChanged = record.document_generation != g_document_generation;
        const bool fallbackKey = documentChanged || recordLineIds.empty() || lineIds.empty();
        if (sameStableLines || fallbackKey) {
            matched = record;
            return true;
        }
    }
    return false;
}

lyric_submit_status submit_lyric_group_with_diagnostics(
    const std::vector<lyric_schedule_item>& schedule,
    const std::vector<size_t>& group, int playbackMs, bool interrupt) {
    if (group.empty() || group.front() >= schedule.size()) return lyric_submit_status::rejected;

    const lyric_schedule_item& item = schedule[group.front()];
    const std::wstring text = schedule_group_text(schedule, group);
    const std::vector<uint64_t> lineIds = schedule_group_line_ids(schedule, group);
    if (text.empty()) return lyric_submit_status::rejected;

    const uint64_t textHash = speaklyrics_log_text_hash(text.c_str());
    const ULONGLONG now = GetTickCount64();
    lyric_submission_record duplicate;
    if (recent_submission_matches(item, lineIds, textHash, now, duplicate)) {
        ++g_track_diagnostics.suspected_duplicates;
        for (const uint64_t lineId : lineIds) {
            g_dispatched_line_ids.insert(lineId);
            g_retry_line_ids.erase(lineId);
        }
        speaklyrics_log_warning(
            L"疑似重复朗读已抑制：歌曲会话=%llu，当前位置世代=%llu，当前歌词文档=%llu，上次歌词文档=%llu，文档序号=%llu，首行ID=%llu，歌词组=%llu行，歌词时间=%d，触发时间=%d，上次任务=%llu，间隔=%llu毫秒，文本哈希=%016llX，内容=%s。",
            static_cast<unsigned long long>(g_track_session_id),
            static_cast<unsigned long long>(g_lyric_position_epoch),
            static_cast<unsigned long long>(g_document_generation),
            static_cast<unsigned long long>(duplicate.document_generation),
            static_cast<unsigned long long>(item.document_index),
            static_cast<unsigned long long>(item.line_id),
            static_cast<unsigned long long>(lineIds.size()), item.lyric_time_ms,
            item.trigger_time_ms,
            static_cast<unsigned long long>(duplicate.speech_task_id),
            static_cast<unsigned long long>(now - duplicate.submitted_at),
            static_cast<unsigned long long>(textHash),
            speaklyrics_log_text_excerpt(text.c_str()).c_str());
        return lyric_submit_status::duplicate;
    }

    ++g_track_diagnostics.planned;
    lyric_speech_diagnostic_context diagnostic;
    diagnostic.track_session = g_track_session_id;
    diagnostic.document_generation = g_document_generation;
    diagnostic.line_index = static_cast<int>(item.document_index);
    diagnostic.line_id = item.line_id;
    diagnostic.line_ids = lineIds;
    diagnostic.position_epoch = g_lyric_position_epoch;
    diagnostic.lyric_time_ms = item.lyric_time_ms;
    diagnostic.trigger_time_ms = item.trigger_time_ms;
    diagnostic.playback_time_ms = playbackMs;
    const speech_enqueue_result enqueueResult = speech_queue_lyric(text.c_str(), interrupt,
        static_cast<unsigned>(lyric_valid_ms()), diagnostic);
    if (!enqueueResult.accepted()) {
        ++g_track_diagnostics.rejected;
        if (enqueueResult.status == speech_enqueue_status::expired) {
            ++g_track_diagnostics.task_expired;
            return lyric_submit_status::expired;
        }
        return lyric_submit_status::rejected;
    }

    ++g_track_diagnostics.accepted;
    lyric_submission_record record;
    record.track_session = g_track_session_id;
    record.document_generation = g_document_generation;
    record.playback_generation = enqueueResult.playback_generation;
    record.speech_task_id = enqueueResult.task_id;
    record.line_index = static_cast<int>(item.document_index);
    record.line_id = item.line_id;
    record.line_ids = lineIds;
    record.position_epoch = g_lyric_position_epoch;
    record.lyric_time_ms = item.lyric_time_ms;
    record.trigger_time_ms = item.trigger_time_ms;
    record.text_hash = textHash;
    record.submitted_at = now;
    record.expires_at = enqueueResult.expires_at;
    g_pending_lyric_submissions.push_back(record);
    g_recent_lyric_submissions.push_back(record);
    prune_recent_lyric_submissions(now);
    for (const uint64_t lineId : lineIds) {
        g_scheduled_line_ids.insert(lineId);
        g_retry_line_ids.erase(lineId);
    }
    g_last_lyric_submission = record;
    g_last_scheduled_line_id = lineIds.empty() ? item.line_id : lineIds.back();
    return lyric_submit_status::accepted;
}

void mark_expired_schedule_item(const lyric_schedule_item& item, int playbackMs,
    const wchar_t* reason) {
    if (item.line_id != 0) {
        g_skipped_line_ids.insert(item.line_id);
        g_retry_line_ids.erase(item.line_id);
    }
    log_skipped_lyric_once(1, item, playbackMs, reason);
}

void speak_for_time(double seconds, bool seekOnly = false) {
    process_speech_task_results();

    if (!cfg_auto_speak.get() || g_paused || g_doc.empty()) return;

    const int offset = static_cast<int>(cfg_lead_ms.get());
    const int playbackMs = static_cast<int>(seconds * 1000.0) - offset;
    const std::vector<lyric_schedule_item> schedule = get_current_lyric_schedule_items();
    if (schedule.empty()) return;

    std::unordered_set<uint64_t> handledLineIds;
    handledLineIds.reserve(g_scheduled_line_ids.size() + g_dispatched_line_ids.size() +
        g_skipped_line_ids.size());
    handledLineIds.insert(g_scheduled_line_ids.begin(), g_scheduled_line_ids.end());
    handledLineIds.insert(g_dispatched_line_ids.begin(), g_dispatched_line_ids.end());
    handledLineIds.insert(g_skipped_line_ids.begin(), g_skipped_line_ids.end());

    auto unhandled_group = [&](const std::vector<size_t>& group) {
        std::vector<size_t> result;
        result.reserve(group.size());
        for (const size_t index : group) {
            if (index >= schedule.size()) continue;
            const uint64_t lineId = schedule[index].line_id;
            if (lineId != 0 && handledLineIds.find(lineId) != handledLineIds.end()) continue;
            result.push_back(index);
        }
        return result;
    };

    auto remember_group = [&](const std::vector<size_t>& group) {
        for (const size_t index : group) {
            if (index >= schedule.size()) continue;
            const uint64_t lineId = schedule[index].line_id;
            if (lineId != 0) handledLineIds.insert(lineId);
        }
    };

    auto retry_group = [&](const std::vector<size_t>& group) {
        for (const size_t index : group) {
            if (index >= schedule.size()) continue;
            const uint64_t lineId = schedule[index].line_id;
            if (lineId != 0) g_retry_line_ids.insert(lineId);
        }
    };

    auto submit_group = [&](const std::vector<size_t>& rawGroup, bool& interrupt) {
        const std::vector<size_t> group = unhandled_group(rawGroup);
        if (group.empty()) return true;

        const lyric_schedule_item& first = schedule[group.front()];
        if (playbackMs - first.lyric_time_ms >= lyric_valid_ms()) {
            for (const size_t index : group) {
                mark_expired_schedule_item(schedule[index], playbackMs,
                    L"超过歌词朗读有效时间");
            }
            remember_group(group);
            return true;
        }

        const lyric_submit_status status = submit_lyric_group_with_diagnostics(
            schedule, group, playbackMs, interrupt);
        if (status == lyric_submit_status::accepted) {
            interrupt = false;
            remember_group(group);
        } else if (status == lyric_submit_status::duplicate) {
            remember_group(group);
        } else if (status == lyric_submit_status::expired) {
            for (const size_t index : group) {
                mark_expired_schedule_item(schedule[index], playbackMs,
                    L"语音任务入队前已经过期");
            }
            remember_group(group);
        } else {
            retry_group(group);
            return false;
        }
        return true;
    };

    if (seekOnly) {
        const std::vector<size_t> group = lyric_scheduler::find_latest_seek_group(schedule, playbackMs);
        g_scheduler_scan_trigger_ms = playbackMs;
        if (group.empty()) return;

        speaklyrics_log_info(
            L"播放跳转调度：歌曲会话=%llu，歌词文档=%llu，位置世代=%llu，目标播放时间=%d，目标歌词时间=%d，目标歌词组=%llu行；只处理目标组，不补读跳过区间。",
            static_cast<unsigned long long>(g_track_session_id),
            static_cast<unsigned long long>(g_document_generation),
            static_cast<unsigned long long>(g_lyric_position_epoch), playbackMs,
            schedule[group.front()].lyric_time_ms,
            static_cast<unsigned long long>(group.size()));

        bool interrupt = true;
        submit_group(group, interrupt);
        return;
    }

    if (playbackMs < g_scheduler_scan_trigger_ms) {
        speaklyrics_log_warning(
            L"歌词调度时间倒退：歌曲会话=%llu，旧扫描时间=%d，当前扫描时间=%d；等待显式跳转事件重新建立播放世代。",
            static_cast<unsigned long long>(g_track_session_id),
            g_scheduler_scan_trigger_ms, playbackMs);
        return;
    }

    const int previousTriggerMs = g_scheduler_scan_trigger_ms;
    const std::vector<size_t> due = lyric_scheduler::collect_due_indices(
        schedule, previousTriggerMs, playbackMs, handledLineIds, g_retry_line_ids);
    if (due.empty()) {
        g_scheduler_scan_trigger_ms = playbackMs;
        return;
    }

    pfc::string8 configuredMode = cfg_lyric_speak_mode.get();
    const bool advanceMode = _stricmp(configuredMode.get_ptr(), "advance") == 0;
    const std::vector<std::vector<size_t>> groups =
        lyric_scheduler::group_indices_by_event(schedule, due);
    log_crossed_lyrics_if_needed(due.size(), groups.size(), previousTriggerMs, playbackMs,
        advanceMode ? L"动态提前朗读" : L"时间戳朗读");

    bool interrupt = true;
    for (size_t groupIndex = 0; groupIndex < groups.size(); ++groupIndex) {
        if (submit_group(groups[groupIndex], interrupt)) continue;

        size_t deferredLines = 0;
        for (size_t remaining = groupIndex + 1; remaining < groups.size(); ++remaining) {
            retry_group(groups[remaining]);
            deferredLines += groups[remaining].size();
        }
        speaklyrics_log_warning(
            L"歌词组入队受阻：歌曲会话=%llu，歌词文档=%llu，位置世代=%llu，受阻组序号=%llu，后续待重试=%llu行；本轮停止提交，避免后面的歌词越过较早歌词。",
            static_cast<unsigned long long>(g_track_session_id),
            static_cast<unsigned long long>(g_document_generation),
            static_cast<unsigned long long>(g_lyric_position_epoch),
            static_cast<unsigned long long>(groupIndex + 1),
            static_cast<unsigned long long>(deferredLines));
        break;
    }
    g_scheduler_scan_trigger_ms = playbackMs;
}



class playback_lyric_speaker : public play_callback_static {

public:

    unsigned get_flags() override {

        return play_callback::flag_on_playback_new_track |

            play_callback::flag_on_playback_stop |

            play_callback::flag_on_playback_seek |

            play_callback::flag_on_playback_pause |

            play_callback::flag_on_playback_edited |

            play_callback::flag_on_playback_time;

    }

    void on_playback_starting(play_control::t_track_command, bool) override {}

    void on_playback_new_track(metadb_handle_ptr p_track) override {

        cancel_playback_background_tasks();
        speech_invalidate_pending(speech_invalidation_reason::track_change);
        process_speech_task_results();
        log_track_diagnostic_summary(L"切换歌曲");
        ++g_track_session_id;
        advance_lyric_position_epoch(L"切换歌曲");
        reset_track_diagnostic_state();

        g_paused = false;

        process_pending_temp_lrc_deletes();

        std::wstring newKey = track_key(p_track);

        if (!g_current_track_key.empty() && !newKey.empty() && newKey != g_current_track_key) {

            schedule_current_temp_lrc_delete();

        }

        g_current_track_key = newKey;
        g_lyric_match_fingerprint = make_lyric_match_fingerprint(p_track);
        g_ignored_metadata_update_logged = false;
        downloader_track_info trackInfo = get_downloader_track_info(p_track);
        speaklyrics_log_info(L"歌词诊断开始：歌曲会话=%llu，标题=%s，艺术家=%s。",
            static_cast<unsigned long long>(g_track_session_id),
            trackInfo.title.c_str(), trackInfo.artist.c_str());

        g_last_missing_lrc_scan_tick = 0;

        g_same_title_candidate_index = 0;
        g_same_title_switch_in_progress = false;
        clear_same_title_candidate_cache();

        load_for_track(p_track, L"切换歌曲");

        bool announced = queue_or_speak_track_announcement(p_track);

        if (!announced) speak_for_time(0);

    }

    void on_playback_stop(play_control::t_stop_reason) override {
        cancel_playback_background_tasks();
        speech_invalidate_pending(speech_invalidation_reason::playback_stop);
        process_speech_task_results();
        advance_lyric_position_epoch(L"停止播放");
        log_track_diagnostic_summary(L"停止播放");

        g_last_missing_lrc_scan_tick = 0;

        clear_same_title_candidate_cache();

        cancel_pending_track_announcement();

    }

    void on_playback_seek(double p_time) override {

        speech_invalidate_pending(speech_invalidation_reason::playback_seek);
        process_speech_task_results();

        advance_lyric_position_epoch(L"播放位置跳转");
        reset_last_spoken(L"播放位置跳转");
        g_last_playback_callback_time = p_time;

        speak_for_time(p_time, true);

    }

    void on_playback_pause(bool p_state) override {

        const bool wasPaused = g_paused;
        g_paused = p_state;

        if (p_state && !wasPaused) {
            cancel_pending_track_announcement();
            speech_invalidate_pending(speech_invalidation_reason::playback_pause);
            process_speech_task_results();
            advance_lyric_position_epoch(L"暂停播放");
            reset_last_spoken(L"暂停播放，等待恢复后重新调度当前歌词");
            return;
        }

        if (!p_state && wasPaused) {
            auto playback = static_api_ptr_t<playback_control>();
            if (!playback->is_playing()) return;

            const double position = playback->playback_get_position();
            g_last_playback_callback_time = position;
            speaklyrics_log_info(
                L"恢复播放调度：歌曲会话=%llu，歌词文档=%llu，位置世代=%llu，播放时间=%.3f；只重新调度当前歌词组。",
                static_cast<unsigned long long>(g_track_session_id),
                static_cast<unsigned long long>(g_document_generation),
                static_cast<unsigned long long>(g_lyric_position_epoch), position);
            speak_for_time(position, true);
        }

    }

    void on_playback_edited(metadb_handle_ptr p_track) override {

        if (p_track.is_empty()) return;

        const std::wstring editedTrackKey = track_key(p_track);
        if (editedTrackKey.empty()) return;

        // on_playback_edited() is expected to describe the current track, but
        // verify it here so a stale notification cannot change the active
        // lyric state. Using the now-playing key also allows a path change to
        // be treated as a relevant matching-field change.
        metadb_handle_ptr nowPlaying;
        const bool hasNowPlaying = static_api_ptr_t<playback_control>()->get_now_playing(nowPlaying);
        const std::wstring nowPlayingKey = hasNowPlaying ? track_key(nowPlaying) : std::wstring();
        if (hasNowPlaying) {
            if (nowPlayingKey.empty() || editedTrackKey != nowPlayingKey) return;
        } else if (editedTrackKey != g_current_track_key) {
            return;
        }

        const lyric_match_fingerprint currentFingerprint = make_lyric_match_fingerprint(p_track);
        const unsigned changedFields = lyric_match_fingerprint_difference(
            g_lyric_match_fingerprint, currentFingerprint);
        if (changedFields == lyric_match_field_none) {
            if (!g_ignored_metadata_update_logged) {
                speaklyrics_log_info(
                    L"标签更新：歌词匹配字段未变化，忽略本次标签通知；评论、评分、播放次数和播放统计等无关字段不会重新加载歌词。");
                g_ignored_metadata_update_logged = true;
            }
            return;
        }

        const bool trackPathChanged = editedTrackKey != g_current_track_key;
        if (trackPathChanged) {
            schedule_current_temp_lrc_delete();
            g_current_track_key = editedTrackKey;
        }
        g_lyric_match_fingerprint = currentFingerprint;
        g_ignored_metadata_update_logged = false;

        speech_invalidate_pending(speech_invalidation_reason::metadata_change);
        process_speech_task_results();
        cancel_playback_background_tasks();

        // Tag edits can supply the missing artist while the same song keeps
        // playing. Drop any title-only temporary candidate and rerun matching
        // immediately with foobar2000's updated metadata.
        delete_current_temp_lrc();
        g_last_missing_lrc_scan_tick = 0;
        g_same_title_candidate_index = 0;
        g_same_title_switch_in_progress = false;
        clear_same_title_candidate_cache();

        downloader_track_info info = get_downloader_track_info(p_track);
        const std::wstring changedFieldNames = lyric_match_field_names(changedFields);
        speaklyrics_log_info(
            L"标签更新：检测到影响歌词匹配的字段发生变化（%s），重新匹配当前歌曲，标题：%s，艺术家：%s。",
            changedFieldNames.c_str(), info.title.c_str(), info.artist.c_str());
        load_for_track(p_track, L"当前歌曲标签更新");

    }

    void on_playback_dynamic_info(const file_info&) override {}

    void on_playback_dynamic_info_track(const file_info&) override {}

    void on_playback_time(double p_time) override {

        if (g_last_playback_callback_time >= 0.0 && p_time >= g_last_playback_callback_time) {
            const int callbackIntervalMs = static_cast<int>((p_time - g_last_playback_callback_time) * 1000.0);
            if (callbackIntervalMs > 2200) {
                ++g_track_diagnostics.delayed_callbacks;
                speaklyrics_log_warning(
                    L"播放时间回调延迟：歌曲会话=%llu，上次时间=%.3f，当前时间=%.3f，间隔=%d毫秒；调度器将检查并补交区间内仍有效的歌词组。",
                    static_cast<unsigned long long>(g_track_session_id),
                    g_last_playback_callback_time, p_time, callbackIntervalMs);
            }
        }
        g_last_playback_callback_time = p_time;

        process_pending_temp_lrc_deletes();

        process_pending_track_announcement();

        retry_load_missing_lrc(p_time);

        if (!g_pending_track_announce_text.empty()) {
            if (!g_pending_announcement_skip_logged && cfg_auto_speak.get() && !g_doc.empty()) {
                ++g_track_diagnostics.announcement_blocks;
                g_pending_announcement_skip_logged = true;
                speaklyrics_log_warning(
                    L"歌词暂缓：歌曲会话=%llu，播放时间=%.3f，原因=等待切换歌曲信息播报；播报结束后会补交仍在有效时间内的歌词组。",
                    static_cast<unsigned long long>(g_track_session_id), p_time);
            }
            return;
        }
        g_pending_announcement_skip_logged = false;

        speak_for_time(p_time);

    }

    void on_volume_change(float) override {}

};



play_callback_static_factory_t<playback_lyric_speaker> g_playback_factory;

}




bool copy_current_lyrics_without_timestamps() {
    const ensure_current_lyrics_result prepared = ensure_current_lyrics_loaded_for_copy();
    if (prepared == ensure_current_lyrics_result::no_playing_track) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u6b63\u5728\u64ad\u653e\u7684\u6b4c\u66f2", true);
        return false;
    }
    if (prepared == ensure_current_lyrics_result::background_download_pending) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u5df2\u52a0\u8f7d\u7684LRC\u6b4c\u8bcd\uff0c\u6b63\u5728\u540e\u53f0\u83b7\u53d6\u6b4c\u8bcd", true);
        return false;
    }
    if (prepared == ensure_current_lyrics_result::unavailable || g_doc.empty()) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u5df2\u52a0\u8f7d\u7684LRC\u6b4c\u8bcd", true);
        return false;
    }

    std::wstring text;
    for (size_t i = 0; i < g_doc.count(); ++i) {
        const lrc_line* line = g_doc.get(i);
        if (!line || line->text.empty()) continue;
        text += line->text;
        text += L"\r\n";
    }

    if (text.empty()) {
        speech_queue_speak(L"\u5f53\u524dLRC\u6ca1\u6709\u53ef\u590d\u5236\u7684\u6b4c\u8bcd\u6587\u672c", true);
        return false;
    }

    bool ok = copy_text_to_clipboard(text);
    speech_queue_speak(ok ? L"\u6b4c\u8bcd\u590d\u5236\u6210\u529f" : L"\u6b4c\u8bcd\u590d\u5236\u5931\u8d25", true);
    return ok;
}

bool speak_current_track_announcement() {
    metadb_handle_ptr track;
    if (!static_api_ptr_t<playback_control>()->get_now_playing(track) || track.is_empty()) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u6b63\u5728\u64ad\u653e\u7684\u6b4c\u66f2", true);
        return false;
    }

    std::wstring text = announce_track_text(track);
    if (text.empty()) {
        speech_queue_speak(L"\u65e0\u6cd5\u83b7\u53d6\u5f53\u524d\u6b4c\u66f2\u4fe1\u606f", true);
        return false;
    }

    cancel_pending_track_announcement();
    speech_queue_speak(text.c_str(), true);
    return true;
}

bool switch_same_title_lyrics(int direction) {
    if (g_same_title_switch_in_progress.exchange(true)) return false;

    const bool previousDirection = direction < 0;
    const int previousCandidateIndex = g_same_title_candidate_index;
    const int candidateIndex = previousCandidateIndex + direction;
    if (candidateIndex < 0) {
        g_same_title_switch_in_progress = false;
        speech_queue_speak(L"\u6ca1\u6709\u4e0a\u4e00\u4e2a\u540c\u540d\u6b4c\u8bcd", true);
        return false;
    }

    metadb_handle_ptr track;
    if (!static_api_ptr_t<playback_control>()->get_now_playing(track) || track.is_empty()) {
        g_same_title_switch_in_progress = false;
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u6b63\u5728\u64ad\u653e\u7684\u6b4c\u66f2", true);
        return false;
    }

    downloader_track_info info = get_downloader_track_info(track);
    std::wstring sources = cfg_path_wide(cfg_lyric_sources);
    std::wstring outputFolder = cfg_path_wide(cfg_temp_lrc_folder);
    fs::path exePath = fs::path(current_dll_dir()) / L"downloader" / L"LrcDownloader.exe";
    if (info.title.empty() || sources.empty() || outputFolder.empty() ||
        !safe_filesystem_exists(exePath, L"同名歌词切换下载器")) {
        g_same_title_switch_in_progress = false;
        speech_queue_speak(L"\u65e0\u6cd5\u5207\u6362\u540c\u540d\u6b4c\u8bcd", true);
        return false;
    }

    std::error_code error;
    fs::create_directories(outputFolder, error);
    if (error) {
        log_filesystem_error_once(L"同名歌词输出目录", L"创建目录",
            fs::path(outputFolder), error);
        g_same_title_switch_in_progress = false;
        speech_queue_speak(L"\u65e0\u6cd5\u5207\u6362\u540c\u540d\u6b4c\u8bcd", true);
        return false;
    }

    g_same_title_candidate_index = candidateIndex;
    const std::wstring requestedTrackKey = track_key(track);
    const std::wstring requestedTitle = info.title;
    const std::wstring candidateCachePath = same_title_candidate_cache_path(requestedTrackKey);
    std::wstring command = command_line_quote(exePath.wstring()) +
        L" --title " + command_line_quote(info.title) +
        L" --album " + command_line_quote(info.album) +
        L" --duration " + std::to_wstring(info.duration_seconds) +
        L" --sources " + command_line_quote(sources) +
        L" --out " + command_line_quote(outputFolder) +
        L" --search-only --title-only --candidate-index " + std::to_wstring(candidateIndex);
    if (!candidateCachePath.empty()) command += L" --candidate-cache " + command_line_quote(candidateCachePath);
    std::wstring manifestPath = temp_lrc_manifest_path();
    if (!manifestPath.empty()) command += L" --manifest " + command_line_quote(manifestPath);

    auto task = speaklyrics_start_background_task(L"same-title lyric switch");
    if (!task) {
        g_same_title_switch_in_progress = false;
        speaklyrics_log_warning(L"Background task skipped during shutdown.");
        return false;
    }
    g_same_title_switch_task = task;
    const uint64_t session = g_track_session_id;
    speaklyrics_run_background_task(task,
        [task, command, exePath, requestedTrackKey, requestedTitle, candidateIndex,
            previousCandidateIndex, previousDirection, session](speaklyrics_background_task& background) {
        const speaklyrics_process_result process = run_process_capture_stdout(
            command, exePath.parent_path(), background.aborter(), 30000, 1024 * 1024);
        std::wstring path;
        std::wstring title;
        std::wstring artist;
        if (process.status == speaklyrics_process_status::completed && process.exit_code == 0) {
            parse_candidate_downloader_output(process.output, path, title, artist);
        }

        const speaklyrics_process_status status = process.status;
        const DWORD processExitCode = process.exit_code;
        const DWORD processErrorCode = process.error_code;
        background.post_to_main_thread([task, requestedTrackKey, requestedTitle, candidateIndex,
            previousCandidateIndex, previousDirection, session, status, processExitCode,
            processErrorCode, path, title, artist]() {
            if (g_same_title_switch_task.get() != task.get()) return;
            g_same_title_switch_task.reset();
            g_same_title_switch_in_progress = false;

            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != requestedTrackKey || g_track_session_id != session) return;

            if (status != speaklyrics_process_status::completed || processExitCode != 0 ||
                path.empty() || !safe_filesystem_exists(
                    fs::path(path), L"同名歌词下载结果")) {
                g_same_title_candidate_index = previousCandidateIndex;
                speaklyrics_log_warning(
                    L"同名歌词切换：进程未完成，状态=%s，退出码=%lu，错误码=%lu，序号：%d。",
                    speaklyrics_process_status_name(status), processExitCode, processErrorCode,
                    candidateIndex);
                speaklyrics_log_warning(L"\u540c\u540d\u6b4c\u8bcd\u5207\u6362\uff1a\u672a\u627e\u5230\u5019\u9009\uff0c\u5e8f\u53f7\uff1a%d\u3002", candidateIndex);
                speech_queue_speak(previousDirection ? L"\u6ca1\u6709\u627e\u5230\u4e0a\u4e00\u4e2a\u540c\u540d\u6b4c\u8bcd" : L"\u6ca1\u6709\u627e\u5230\u4e0b\u4e00\u4e2a\u540c\u540d\u6b4c\u8bcd", true);
                return;
            }

            lrc_document candidateDocument;
            pfc::string8 loadError;
            if (!candidateDocument.load(path, loadError)) {
                g_same_title_candidate_index = previousCandidateIndex;
                speech_queue_speak(previousDirection ? L"\u6ca1\u6709\u627e\u5230\u4e0a\u4e00\u4e2a\u540c\u540d\u6b4c\u8bcd" : L"\u6ca1\u6709\u627e\u5230\u4e0b\u4e00\u4e2a\u540c\u540d\u6b4c\u8bcd", true);
                return;
            }

            schedule_current_temp_lrc_delete();
            speech_invalidate_pending(speech_invalidation_reason::same_title_lyrics);
            process_speech_task_results();
            reset_last_spoken(L"切换同名歌词候选");
            g_doc = std::move(candidateDocument);
            g_current_lrc = path;
            g_current_lrc_temporary = true;
            g_loaded_lrc_track_key = requestedTrackKey;
            mark_document_loaded(L"同名歌词候选");
            cancel_pending_temp_lrc_delete(g_current_lrc);
            refresh_lyrics_jump_window();

            std::wstring announcement = title.empty() ? requestedTitle : title;
            if (!artist.empty()) announcement += L"\uff0c" + artist;
            speaklyrics_log_info(L"\u540c\u540d\u6b4c\u8bcd\u5207\u6362\uff1a\u5df2\u52a0\u8f7d\uff0c\u6807\u9898\uff1a%s\uff0c\u827a\u672f\u5bb6\uff1a%s\u3002", announcement.c_str(), artist.c_str());
            speech_queue_speak(announcement.c_str(), true);
        });
    });
    return true;
}

bool switch_to_next_same_title_lyrics() {
    return switch_same_title_lyrics(1);
}

bool switch_to_previous_same_title_lyrics() {
    return switch_same_title_lyrics(-1);
}

void reload_current_lyrics() {

    metadb_handle_ptr track;

    if (static_api_ptr_t<playback_control>()->get_now_playing(track)) {

        cancel_playback_background_tasks();
        speech_invalidate_pending(speech_invalidation_reason::lyrics_reload);
        process_speech_task_results();
        load_for_track(track, L"手动刷新或设置变更");

    }

}

ensure_current_lyrics_result ensure_current_lyrics_loaded_for_copy() {
    metadb_handle_ptr track;
    if (!static_api_ptr_t<playback_control>()->get_now_playing(track) || track.is_empty()) {
        speaklyrics_log_warning(L"复制歌词准备：当前没有正在播放的歌曲。");
        return ensure_current_lyrics_result::no_playing_track;
    }

    const std::wstring currentTrackKey = track_key(track);
    if (currentTrackKey.empty()) {
        speaklyrics_log_warning(L"复制歌词准备：无法取得当前歌曲标识，不读取可能属于上一首歌曲的内存歌词。");
        return ensure_current_lyrics_result::unavailable;
    }

    if (!g_doc.empty() && g_loaded_lrc_track_key == currentTrackKey) {
        return ensure_current_lyrics_result::already_loaded;
    }

    process_pending_temp_lrc_deletes();
    const bool replacingStaleDocument = !g_doc.empty() || !g_loaded_lrc_track_key.empty();
    speaklyrics_log_info(
        L"复制歌词准备：当前歌词为空或不属于正在播放的歌曲，开始同步检查已有本地、标签和临时歌词；内存歌词行=%llu，当前歌曲匹配=%s。",
        static_cast<unsigned long long>(g_doc.count()),
        g_loaded_lrc_track_key == currentTrackKey ? L"是" : L"否");

    const auto found = find_lrc_for_track(track);
    if (!found) {
        maybe_start_lrc_downloader(track);
        const bool downloadPending = lrc_download_in_progress_for(currentTrackKey);
        speaklyrics_log_warning(
            L"复制歌词准备：同步检查未找到可用歌词，后台下载状态=%s。",
            downloadPending ? L"正在进行" : L"未启动");
        return downloadPending
            ? ensure_current_lyrics_result::background_download_pending
            : ensure_current_lyrics_result::unavailable;
    }

    lrc_document candidateDocument;
    pfc::string8 loadError;
    const bool embedded = !found->embedded_text.empty();
    const bool loaded = embedded
        ? candidateDocument.load_text(found->embedded_text, L"<LYRICS>", loadError)
        : candidateDocument.load(found->path, loadError);
    if (!loaded) {
        if (embedded) {
            speaklyrics_log_error(
                L"复制歌词准备：内嵌 LYRICS 标签解析失败：%s。",
                pfc::stringcvt::string_wide_from_utf8(loadError.get_ptr()).get_ptr());
        } else {
            speaklyrics_log_error(
                L"复制歌词准备：候选 LRC 解析失败：%s，文件：%s。",
                pfc::stringcvt::string_wide_from_utf8(loadError.get_ptr()).get_ptr(),
                found->path.c_str());
        }
        maybe_start_lrc_downloader(track);
        const bool downloadPending = lrc_download_in_progress_for(currentTrackKey);
        return downloadPending
            ? ensure_current_lyrics_result::background_download_pending
            : ensure_current_lyrics_result::unavailable;
    }

    if (replacingStaleDocument) {
        speech_invalidate_pending(speech_invalidation_reason::lyrics_reload);
        process_speech_task_results();
        reset_last_spoken(L"复制歌词前发现旧歌曲或失效歌词状态");
    }

    schedule_current_temp_lrc_delete();
    g_doc = std::move(candidateDocument);
    g_current_lrc = embedded ? std::wstring() : found->path;
    g_current_lrc_temporary = !embedded && found->temporary;
    g_loaded_lrc_track_key = currentTrackKey;

    const wchar_t* source = embedded
        ? L"复制前刷新：音频文件内嵌 LYRICS 标签"
        : (found->temporary
            ? L"复制前刷新：临时歌词目录 LRC"
            : L"复制前刷新：本地或指定目录 LRC");
    mark_document_loaded(source);
    if (g_current_lrc_temporary) cancel_pending_temp_lrc_delete(g_current_lrc);
    refresh_lyrics_jump_window();

    if (embedded) {
        speaklyrics_log_info(
            L"复制歌词准备：已同步加载当前歌曲的内嵌 LYRICS 标签，共 %llu 行。",
            static_cast<unsigned long long>(g_doc.count()));
    } else {
        speaklyrics_log_info(
            L"复制歌词准备：已同步加载当前歌曲的 LRC，共 %llu 行，文件：%s。",
            static_cast<unsigned long long>(g_doc.count()), g_current_lrc.c_str());
    }
    return ensure_current_lyrics_result::loaded;
}



void bind_manual_lrc_to_current_track() {

    metadb_handle_ptr track;

    if (static_api_ptr_t<playback_control>()->get_now_playing(track)) {

        g_manual_lrc_track_key = track_key(track);

    } else {

        g_manual_lrc_track_key.clear();

    }

}



void set_manual_lrc_file_for_current_track(const char* path) {

    cfg_lrc_file.set(path ? path : "");

    bind_manual_lrc_to_current_track();

}

void cancel_playback_background_tasks() {
    if (g_lrc_downloader_task) {
        g_lrc_downloader_task->cancel();
        g_lrc_downloader_task.reset();
    }
    g_lrc_download_state.reset();
    if (g_same_title_prefetch_task) {
        g_same_title_prefetch_task->cancel();
        g_same_title_prefetch_task.reset();
    }
    if (g_same_title_switch_task) {
        g_same_title_switch_task->cancel();
        g_same_title_switch_task.reset();
    }
    g_same_title_switch_in_progress = false;
}

std::vector<lyric_jump_item> get_current_lyric_jump_items() {
    std::vector<lyric_jump_item> items;
    if (g_doc.empty()) return items;

    for (size_t i = 0; i < g_doc.count(); ++i) {
        const lrc_line* line = g_doc.get(i);
        if (!line || line->text.empty()) continue;
        items.push_back({ line->time_ms, line->text, line->line_id, i });
    }
    return items;
}

std::vector<lyric_schedule_item> get_current_lyric_schedule_items() {
    std::vector<lyric_jump_item> sourceItems = get_current_lyric_jump_items();
    if (sourceItems.empty()) return {};

    pfc::string8 configuredMode = cfg_lyric_speak_mode.get();
    const bool advanceMode = _stricmp(configuredMode.get_ptr(), "advance") == 0;
    if (advanceMode) sourceItems = filter_leading_lyric_credits(sourceItems);

    std::vector<lyric_schedule_item> schedule;
    schedule.reserve(sourceItems.size());

    if (!advanceMode) {
        int groupTime = -1;
        size_t groupOrder = 0;
        for (const auto& item : sourceItems) {
            if (item.time_ms != groupTime) {
                groupTime = item.time_ms;
                groupOrder = 0;
            }
            schedule.push_back({ item.line_id, item.document_index, item.time_ms,
                item.time_ms, groupOrder++, item.text });
        }
        return schedule;
    }

    bool havePreviousGroup = false;
    int previousGroupLyricTime = 0;
    for (size_t first = 0; first < sourceItems.size();) {
        size_t last = first + 1;
        while (last < sourceItems.size() &&
            sourceItems[last].time_ms == sourceItems[first].time_ms) {
            ++last;
        }

        std::wstring groupText;
        for (size_t index = first; index < last; ++index) {
            if (!groupText.empty()) groupText += L"\r\n";
            groupText += sourceItems[index].text;
        }

        const int lyricTime = sourceItems[first].time_ms;
        int triggerTime = (std::max)(0, lyricTime - estimate_lyric_speech_ms(groupText));
        if (havePreviousGroup) {
            triggerTime = (std::max)(triggerTime, previousGroupLyricTime);
        }

        size_t sameTimestampOrder = 0;
        for (size_t index = first; index < last; ++index) {
            const auto& item = sourceItems[index];
            schedule.push_back({ item.line_id, item.document_index, item.time_ms,
                triggerTime, sameTimestampOrder++, item.text });
        }

        previousGroupLyricTime = lyricTime;
        havePreviousGroup = true;
        first = last;
    }

    std::stable_sort(schedule.begin(), schedule.end(),
        [](const lyric_schedule_item& first, const lyric_schedule_item& second) {
            return first.trigger_time_ms < second.trigger_time_ms;
        });
    return schedule;
}

bool jump_to_lyric_time_ms(int time_ms) {
    if (time_ms < 0) time_ms = 0;

    auto playback = static_api_ptr_t<playback_control>();
    if (!playback->is_playing()) {
        playback->start(playback_control::track_command_play, false);
    } else if (playback->is_paused()) {
        playback->pause(false);
    }

    playback->playback_seek(static_cast<double>(time_ms) / 1000.0);
    return true;
}


current_track_search_info get_current_track_search_info() {
    current_track_search_info out;
    static_api_ptr_t<playback_control> playback;
    metadb_handle_ptr track;
    if (!playback->get_now_playing(track) || track.is_empty()) return out;
    downloader_track_info info = get_downloader_track_info(track);
    out.title = info.title;
    out.artist = info.artist;
    out.album = info.album;
    out.duration_seconds = info.duration_seconds;
    return out;
}
