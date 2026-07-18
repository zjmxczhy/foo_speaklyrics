#include "stdafx.h"

#include "config.h"

#include "lrc_parser.h"
#include "playback.h"
#include "lyrics_copy.h"
#include "lyrics_jump_window.h"

#include "speech_engine.h"
#include "speaklyrics_log.h"
#include "temp_lrc_manifest.h"



namespace {

lrc_document g_doc;

int g_last_spoken = -1;

std::atomic_bool g_same_title_switch_in_progress{ false };
int g_same_title_candidate_index = 0;
std::wstring g_same_title_candidate_cache_path;
std::wstring g_same_title_candidate_cache_key;
std::wstring g_same_title_prefetch_requested_key;

bool g_paused = false;

std::wstring g_current_lrc;

bool g_current_lrc_temporary = false;

std::wstring g_manual_lrc_track_key;

double g_last_missing_lrc_retry_time = -1000.0;

std::wstring g_downloader_requested_track_key;

std::wstring g_current_track_key;

std::wstring g_pending_track_announce_text;

ULONGLONG g_pending_track_announce_due_tick = 0;

struct pending_temp_lrc_delete {
    std::wstring path;
    ULONGLONG due_tick = 0;
};

std::vector<pending_temp_lrc_delete> g_pending_temp_lrc_deletes;

void cancel_pending_temp_lrc_delete(const std::wstring& path);
void schedule_current_temp_lrc_delete();



struct lrc_match {

    std::wstring path;

    bool temporary;

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

bool run_process_capture_stdout(const std::wstring& command, const fs::path& workDir, std::string& stdoutText, DWORD& exitCode) {
    stdoutText.clear();
    exitCode = 3;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return false;
    }

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        stdoutText.append(buffer, buffer + read);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return true;
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
    if (error || folder.empty()) return std::wstring();
    const size_t keyHash = std::hash<std::wstring>{}(key);
    fs::path path = folder / (L"foo_speaklyrics-candidates-" + std::to_wstring(static_cast<unsigned long long>(keyHash)) + L".json");
    g_same_title_candidate_cache_key = key;
    g_same_title_candidate_cache_path = path.wstring();
    return g_same_title_candidate_cache_path;
}

void maybe_prefetch_same_title_candidates(metadb_handle_ptr track) {
    if (track.is_empty()) return;
    downloader_track_info info = get_downloader_track_info(track);
    if (!info.artist.empty() && !info.metadata_corrected) return;

    const std::wstring key = track_key(track);
    if (key.empty() || g_same_title_prefetch_requested_key == key) return;
    const std::wstring cachePath = same_title_candidate_cache_path(key);
    const std::wstring sources = cfg_path_wide(cfg_lyric_sources);
    fs::path exePath = fs::path(current_dll_dir()) / L"downloader" / L"LrcDownloader.exe";
    if (cachePath.empty() || sources.empty() || info.title.empty() || !fs::exists(exePath)) return;

    g_same_title_prefetch_requested_key = key;
    std::wstring command = command_line_quote(exePath.wstring()) +
        L" --title " + command_line_quote(info.title) +
        L" --artist " + command_line_quote(info.artist) +
        L" --album " + command_line_quote(info.album) +
        L" --duration " + std::to_wstring(info.duration_seconds) +
        L" --sources " + command_line_quote(sources) +
        L" --search-only --title-only --list --candidate-cache " + command_line_quote(cachePath);

    std::thread([command, exePath, key, cachePath]() {
        std::string output;
        DWORD exitCode = 3;
        const bool started = run_process_capture_stdout(command, exePath.parent_path(), output, exitCode);
        fb2k::inMainThread([key, cachePath, started, exitCode]() {
            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != key || g_current_track_key != key) {
                std::error_code error;
                fs::remove(cachePath, error);
                return;
            }
            if (started && exitCode == 0 && fs::exists(cachePath)) {
                speaklyrics_log_info(L"同名歌词候选：已完成后台预取。");
            } else {
                speaklyrics_log_warning(L"同名歌词候选：后台预取未完成。");
            }
        });
    }).detach();
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
        speech_queue_speak(text.c_str(), true);
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
    speech_queue_speak(text.c_str(), true);
}



void maybe_start_lrc_downloader(metadb_handle_ptr track) {

    if (track.is_empty()) return;



    std::wstring key = track_key(track);

    if (key.empty() || g_downloader_requested_track_key == key) return;



    std::wstring sources = cfg_path_wide(cfg_lyric_sources);

    if (sources.empty()) return;



    std::wstring permanentFolder = cfg_path_wide(cfg_lrc_folder);
    bool temporaryDownload = trim_text(permanentFolder).empty();
    std::wstring outputFolder = temporaryDownload ? cfg_path_wide(cfg_temp_lrc_folder) : permanentFolder;

    if (outputFolder.empty()) return;



    std::error_code ec;

    fs::create_directories(outputFolder, ec);

    if (ec || !fs::is_directory(outputFolder, ec)) {

        FB2K_console_formatter() << "foo_speaklyrics: lrc download output folder is not available: " << pfc::stringcvt::string_utf8_from_wide(outputFolder.c_str()).get_ptr();
        speaklyrics_log_error(L"自动下载：输出目录不可用：%s。", outputFolder.c_str());

        g_downloader_requested_track_key = key;

        return;

    }



    fs::path exePath = fs::path(current_dll_dir()) / L"downloader" / L"LrcDownloader.exe";

    if (!fs::exists(exePath, ec)) {

        FB2K_console_formatter() << "foo_speaklyrics: downloader not found: " << pfc::stringcvt::string_utf8_from_wide(exePath.c_str()).get_ptr();
        speaklyrics_log_error(L"自动下载：找不到歌词下载器：%s。", exePath.c_str());

        g_downloader_requested_track_key = key;

        return;

    }



    downloader_track_info info = get_downloader_track_info(track);

    if (info.title.empty()) {

        FB2K_console_formatter() << "foo_speaklyrics: downloader skipped because track title is empty";
        speaklyrics_log_warning(L"自动下载：跳过下载，当前歌曲标题为空。");

        g_downloader_requested_track_key = key;

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



    g_downloader_requested_track_key = key;
    FB2K_console_formatter() << "foo_speaklyrics: started lrc downloader for " << pfc::stringcvt::string_utf8_from_wide(info.title.c_str()).get_ptr();
    speaklyrics_log_info(L"自动下载：已启动下载器，标题：%s，艺术家：%s，来源：%s。", info.title.c_str(), info.artist.c_str(), sources.c_str());

    std::thread([command, exePath, key, temporaryDownload]() {
        std::string output;
        DWORD exitCode = 3;
        const bool started = run_process_capture_stdout(command, exePath.parent_path(), output, exitCode);
        std::wstring downloadedPath;
        std::wstring selectedTitle;
        std::wstring selectedArtist;
        if (started && exitCode == 0) {
            parse_candidate_downloader_output(output, downloadedPath, selectedTitle, selectedArtist);
        }

        fb2k::inMainThread([key, temporaryDownload, started, exitCode, downloadedPath, selectedTitle, selectedArtist]() {
            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != key) return;

            if (!started) {
                speaklyrics_log_error(L"自动下载：无法启动下载器。");
                return;
            }
            if (exitCode != 0 || downloadedPath.empty() || !fs::exists(downloadedPath)) {
                speaklyrics_log_warning(L"自动下载：未找到可用 LRC。");
                return;
            }

            lrc_document downloadedDocument;
            pfc::string8 loadError;
            if (!downloadedDocument.load(downloadedPath, loadError)) {
                speaklyrics_log_error(L"歌词加载：下载的 LRC 解析失败：%s，文件：%s。",
                    pfc::stringcvt::string_wide_from_utf8(loadError.get_ptr()).get_ptr(), downloadedPath.c_str());
                return;
            }

            schedule_current_temp_lrc_delete();
            g_doc = std::move(downloadedDocument);
            g_current_lrc = downloadedPath;
            g_current_lrc_temporary = temporaryDownload;
            g_last_spoken = -1;
            if (g_current_lrc_temporary) cancel_pending_temp_lrc_delete(g_current_lrc);
            refresh_lyrics_jump_window();

            FB2K_console_formatter() << "foo_speaklyrics: loaded downloaded lrc "
                << pfc::stringcvt::string_utf8_from_wide(downloadedPath.c_str()).get_ptr();
            speaklyrics_log_info(L"歌词加载：已直接加载下载的 LRC：%s，标题：%s，艺术家：%s。",
                downloadedPath.c_str(), selectedTitle.c_str(), selectedArtist.c_str());
            maybe_prefetch_same_title_candidates(currentTrack);
        });
    }).detach();

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



std::optional<std::wstring> find_lrc_in_folder(const std::wstring& folder, metadb_handle_ptr track, const std::optional<std::wstring>& trackPath) {

    if (folder.empty() || !fs::is_directory(folder)) return std::nullopt;



    fs::path base;

    if (trackPath) {

        base = fs::path(*trackPath);

        fs::path candidate = fs::path(folder) / (base.stem().wstring() + L".lrc");

        if (fs::exists(candidate)) return candidate.wstring();

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

            fs::path candidate = fs::path(folder) / name;

            if (fs::exists(candidate)) return candidate.wstring();

            name = titleText + L" - " + artistText + L".lrc";

            candidate = fs::path(folder) / name;

            if (fs::exists(candidate)) return candidate.wstring();

        }

    }



    const std::wstring normalizedStem = trackPath ? normalize_match_text(base.stem().wstring()) : std::wstring();

    const std::wstring normalizedArtist = normalize_match_text(artistText);

    const std::wstring normalizedTitle = normalize_match_text(titleText);



    std::optional<std::wstring> best;

    int bestScore = 0;

    size_t bestNameLength = static_cast<size_t>(-1);

    std::error_code ec;

    for (const auto& entry : fs::directory_iterator(folder, ec)) {

        if (ec) break;

        if (!entry.is_regular_file(ec)) continue;

        fs::path path = entry.path();

        if (_wcsicmp(path.extension().c_str(), L".lrc") != 0) continue;



        std::wstring normalizedName = normalize_match_text(path.stem().wstring());

        int score = 0;

        if (!normalizedArtist.empty() && !normalizedTitle.empty()) {

            // When an artist is known, never reuse another artist's same-title
            // lyric from a shared folder. This keeps local matching consistent
            // with downloader matching: title + artist first, title-only only
            // when the artist is genuinely unavailable.
            if (contains_match_text(normalizedName, normalizedArtist) && contains_match_text(normalizedName, normalizedTitle)) score = 4;

        } else if (!normalizedTitle.empty() && contains_match_text(normalizedName, normalizedTitle)) {

            score = 3;

        } else if (!normalizedStem.empty() && contains_match_text(normalizedName, normalizedStem)) {

            score = 2;

        } else if (!normalizedName.empty() && contains_match_text(normalizedStem, normalizedName)) {

            score = 1;

        }



        size_t nameLength = normalizedName.size();

        if (score > bestScore || (score == bestScore && score > 0 && nameLength < bestNameLength)) {

            best = path.wstring();

            bestScore = score;

            bestNameLength = nameLength;

        }

    }



    return best;

}



std::optional<lrc_match> find_lrc_for_track(metadb_handle_ptr track) {

    std::wstring manual = cfg_path_wide(cfg_lrc_file);

    if (!manual.empty() && !g_manual_lrc_track_key.empty() && g_manual_lrc_track_key == track_key(track) && fs::exists(manual)) {

        return lrc_match{ manual, false };

    }



    auto trackPath = local_track_path(track);

    if (trackPath) {

        fs::path trackFolder = fs::path(*trackPath).parent_path();

        if (auto local = find_lrc_in_folder(trackFolder.wstring(), track, trackPath)) return lrc_match{ *local, false };

    }



    std::wstring folder = cfg_path_wide(cfg_lrc_folder);

    if (auto normal = find_lrc_in_folder(folder, track, trackPath)) return lrc_match{ *normal, false };



    std::wstring tempFolder = cfg_path_wide(cfg_temp_lrc_folder);

    if (auto temp = find_lrc_in_folder(tempFolder, track, trackPath)) return lrc_match{ *temp, true };



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



void clear_loaded_lrc() {

    g_doc.clear();

    g_current_lrc.clear();

    g_current_lrc_temporary = false;

    g_last_spoken = -1;

}



void load_for_track(metadb_handle_ptr track);



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



bool retry_load_missing_lrc(double seconds) {

    if (!g_doc.empty() || g_paused) return false;

    if ((seconds - g_last_missing_lrc_retry_time) * 1000.0 < static_cast<double>(missing_lrc_retry_ms())) return false;

    g_last_missing_lrc_retry_time = seconds;



    metadb_handle_ptr track;

    if (!static_api_ptr_t<playback_control>()->get_now_playing(track)) return false;



    load_for_track(track);

    return !g_doc.empty();

}



void load_for_track(metadb_handle_ptr track) {

    process_pending_temp_lrc_deletes();

    clear_loaded_lrc();

    auto found = find_lrc_for_track(track);

    if (!found) {

        speaklyrics_log_warning(L"歌词加载：未找到可用 LRC，准备按设置尝试下载。");
        maybe_start_lrc_downloader(track);

        refresh_lyrics_jump_window();

        return;

    }

    pfc::string8 error;

    if (g_doc.load(found->path, error)) {

        g_current_lrc = found->path;

        g_current_lrc_temporary = found->temporary;

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

void speak_for_time(double seconds) {

    if (!cfg_auto_speak.get() || g_paused || g_doc.empty()) return;

    int offset = static_cast<int>(cfg_lead_ms.get());

    int ms = static_cast<int>(seconds * 1000.0) - offset;

    int currentIndex = g_doc.find_index_for_time(ms);
    int spokenIndex = currentIndex;
    pfc::string8 configuredMode = cfg_lyric_speak_mode.get();
    const bool advanceMode = _stricmp(configuredMode.get_ptr(), "advance") == 0;

    if (advanceMode) {
        std::vector<lyric_jump_item> items = filter_leading_lyric_credits(get_current_lyric_jump_items());
        spokenIndex = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            int speakTime = (std::max)(0, items[i].time_ms - estimate_lyric_speech_ms(items[i].text));
            if (i > 0) speakTime = (std::max)(speakTime, items[i - 1].time_ms);
            if (speakTime <= ms) spokenIndex = static_cast<int>(i);
        }
        if (spokenIndex < 0 || spokenIndex == g_last_spoken) return;

        const lyric_jump_item& item = items[static_cast<size_t>(spokenIndex)];
        if (ms - item.time_ms > lyric_valid_ms()) return;
        speech_queue_lyric(item.text.c_str(), true, static_cast<unsigned>(lyric_valid_ms()));
        g_last_spoken = spokenIndex;
        return;
    } else {
        if (currentIndex < 0) return;
        const lrc_line* triggerLine = g_doc.get(static_cast<size_t>(currentIndex));
        if (triggerLine && ms - triggerLine->time_ms > lyric_valid_ms()) return;
    }

    if (spokenIndex < 0 || spokenIndex == g_last_spoken) return;
    const lrc_line* line = g_doc.get(static_cast<size_t>(spokenIndex));

    if (line && !line->text.empty()) {

        speech_queue_lyric(line->text.c_str(), true, static_cast<unsigned>(lyric_valid_ms()));

        g_last_spoken = spokenIndex;

    }

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

        speech_invalidate_pending();

        g_paused = false;

        process_pending_temp_lrc_deletes();

        std::wstring newKey = track_key(p_track);

        if (!g_current_track_key.empty() && !newKey.empty() && newKey != g_current_track_key) {

            schedule_current_temp_lrc_delete();

        }

        g_current_track_key = newKey;

        g_last_missing_lrc_retry_time = -1000.0;

        g_downloader_requested_track_key.clear();

        g_same_title_candidate_index = 0;
        g_same_title_switch_in_progress = false;
        clear_same_title_candidate_cache();

        load_for_track(p_track);

        bool announced = queue_or_speak_track_announcement(p_track);

        if (!announced) speak_for_time(0);

    }

    void on_playback_stop(play_control::t_stop_reason) override {

        g_last_missing_lrc_retry_time = -1000.0;

        g_downloader_requested_track_key.clear();

        clear_same_title_candidate_cache();

        cancel_pending_track_announcement();

        speech_queue_silence();

    }

    void on_playback_seek(double p_time) override {

        speech_invalidate_pending();

        g_last_spoken = -1;

        speak_for_time(p_time);

    }

    void on_playback_pause(bool p_state) override {

        g_paused = p_state;

        if (p_state) {
            cancel_pending_track_announcement();
            speech_queue_silence();
        }

    }

    void on_playback_edited(metadb_handle_ptr p_track) override {

        if (p_track.is_empty() || track_key(p_track) != g_current_track_key) return;

        speech_invalidate_pending();

        // Tag edits can supply the missing artist while the same song keeps
        // playing. Drop any title-only temporary candidate and rerun matching
        // immediately with foobar2000's updated metadata.
        delete_current_temp_lrc();
        clear_loaded_lrc();
        g_last_missing_lrc_retry_time = -1000.0;
        g_downloader_requested_track_key.clear();
        g_same_title_candidate_index = 0;
        g_same_title_switch_in_progress = false;
        clear_same_title_candidate_cache();

        downloader_track_info info = get_downloader_track_info(p_track);
        speaklyrics_log_info(L"标签更新：重新匹配当前歌曲，标题：%s，艺术家：%s。", info.title.c_str(), info.artist.c_str());
        load_for_track(p_track);

    }

    void on_playback_dynamic_info(const file_info&) override {}

    void on_playback_dynamic_info_track(const file_info&) override {}

    void on_playback_time(double p_time) override {

        process_pending_temp_lrc_deletes();

        process_pending_track_announcement();

        retry_load_missing_lrc(p_time);

        if (!g_pending_track_announce_text.empty()) return;

        speak_for_time(p_time);

    }

    void on_volume_change(float) override {}

};



play_callback_static_factory_t<playback_lyric_speaker> g_playback_factory;

}




bool copy_current_lyrics_without_timestamps() {
    if (g_doc.empty()) {
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
    if (info.title.empty() || sources.empty() || outputFolder.empty() || !fs::exists(exePath)) {
        g_same_title_switch_in_progress = false;
        speech_queue_speak(L"\u65e0\u6cd5\u5207\u6362\u540c\u540d\u6b4c\u8bcd", true);
        return false;
    }

    std::error_code error;
    fs::create_directories(outputFolder, error);
    if (error) {
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
        L" --artist " + command_line_quote(info.artist) +
        L" --album " + command_line_quote(info.album) +
        L" --duration " + std::to_wstring(info.duration_seconds) +
        L" --sources " + command_line_quote(sources) +
        L" --out " + command_line_quote(outputFolder) +
        L" --search-only --title-only --candidate-index " + std::to_wstring(candidateIndex);
    if (!candidateCachePath.empty()) command += L" --candidate-cache " + command_line_quote(candidateCachePath);
    std::wstring manifestPath = temp_lrc_manifest_path();
    if (!manifestPath.empty()) command += L" --manifest " + command_line_quote(manifestPath);

    std::thread([command, exePath, requestedTrackKey, requestedTitle, candidateIndex, previousCandidateIndex, previousDirection]() {
        std::string output;
        DWORD exitCode = 3;
        bool started = run_process_capture_stdout(command, exePath.parent_path(), output, exitCode);
        std::wstring path;
        std::wstring title;
        std::wstring artist;
        if (started && exitCode == 0) parse_candidate_downloader_output(output, path, title, artist);

        fb2k::inMainThread([requestedTrackKey, requestedTitle, candidateIndex, previousCandidateIndex, previousDirection, exitCode, path, title, artist]() {
            g_same_title_switch_in_progress = false;

            metadb_handle_ptr currentTrack;
            if (!static_api_ptr_t<playback_control>()->get_now_playing(currentTrack) ||
                track_key(currentTrack) != requestedTrackKey) return;

            if (exitCode != 0 || path.empty() || !fs::exists(path)) {
                g_same_title_candidate_index = previousCandidateIndex;
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
            g_doc = std::move(candidateDocument);
            g_current_lrc = path;
            g_current_lrc_temporary = true;
            g_last_spoken = -1;
            cancel_pending_temp_lrc_delete(g_current_lrc);
            refresh_lyrics_jump_window();

            std::wstring announcement = title.empty() ? requestedTitle : title;
            if (!artist.empty()) announcement += L"\uff0c" + artist;
            speaklyrics_log_info(L"\u540c\u540d\u6b4c\u8bcd\u5207\u6362\uff1a\u5df2\u52a0\u8f7d\uff0c\u6807\u9898\uff1a%s\uff0c\u827a\u672f\u5bb6\uff1a%s\u3002", announcement.c_str(), artist.c_str());
            speech_queue_speak(announcement.c_str(), true);
        });
    }).detach();
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

        load_for_track(track);

    }

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

    reload_current_lyrics();

}

std::vector<lyric_jump_item> get_current_lyric_jump_items() {
    std::vector<lyric_jump_item> items;
    if (g_doc.empty()) return items;

    for (size_t i = 0; i < g_doc.count(); ++i) {
        const lrc_line* line = g_doc.get(i);
        if (!line || line->text.empty()) continue;
        items.push_back({ line->time_ms, line->text });
    }
    return items;
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
    g_last_spoken = -1;
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
