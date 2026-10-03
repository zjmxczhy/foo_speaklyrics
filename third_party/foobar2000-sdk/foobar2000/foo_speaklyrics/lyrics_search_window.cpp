#include "stdafx.h"

#include "lyrics_search_window.h"
#include "background_task.h"
#include "config.h"
#include "playback.h"
#include "process_runner.h"
#include "resource.h"
#include "speech_engine.h"
#include "speaklyrics_log.h"
#include "temp_lrc_manifest.h"

#include <cwctype>
#include <sstream>
#include <windowsx.h>

namespace {

HWND g_window = nullptr;
HWND g_auto_button = nullptr;
HWND g_title_edit = nullptr;
HWND g_artist_edit = nullptr;
HWND g_search_button = nullptr;
HWND g_title_label = nullptr;
HWND g_artist_label = nullptr;
HWND g_progress = nullptr;
HWND g_list = nullptr;
HWND g_close_button = nullptr;
bool g_searching = false;
uint64_t g_window_generation = 0;
uint64_t g_search_request_id = 0;
uint64_t g_download_request_id = 0;
uint64_t g_manual_candidate_cache_id = 0;
std::wstring g_manual_candidate_cache_path;
speaklyrics_background_task_ptr g_search_task;
speaklyrics_background_task_ptr g_download_task;

struct search_result_item {
    std::wstring title;
    std::wstring artist;
    std::wstring source_key;
    std::wstring source_name;
    int candidate_index = -1;
    std::wstring candidate_cache_path;
    std::wstring query_title;
    std::wstring query_artist;
    std::wstring query_album;
    std::wstring query_sources;
    int query_duration_seconds = 0;
    bool query_title_only = false;
    bool placeholder = false;
};

std::vector<search_result_item> g_items;

bool window_contains_focus(HWND window, HWND focus) {
    return window && focus && (focus == window || IsChild(window, focus));
}

void set_accessible_name(HWND wnd, const wchar_t* name) {
    if (!wnd) return;
    SetPropW(wnd, L"Name", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(name)));
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, wnd, OBJID_CLIENT, CHILDID_SELF);
}

std::wstring utf8_to_wide_local(const std::string& value) {
    if (value.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

std::string wide_to_utf8_local(const std::wstring& value) {
    if (value.empty()) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

pfc::string8 wide_to_pfc_utf8(const std::wstring& value) {
    return pfc::stringcvt::string_utf8_from_wide(value.c_str()).get_ptr();
}

std::wstring cfg_to_wide_local(cfg_string& s) {
    pfc::string8 v = s.get();
    return pfc::stringcvt::string_wide_from_utf8(v).get_ptr();
}

std::wstring get_window_text(HWND wnd) {
    if (!wnd) return std::wstring();
    int len = GetWindowTextLengthW(wnd);
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(wnd, out.data(), len + 1);
    out.resize(static_cast<size_t>(len));
    return out;
}

std::wstring trim_text(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring normalize_for_match(const std::wstring& text) {
    std::wstring out;
    for (wchar_t ch : text) {
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) {
            out.push_back(static_cast<wchar_t>(towlower(ch)));
        } else if (ch > 127) {
            out.push_back(ch);
        }
    }
    return out;
}

bool same_match(const std::wstring& a, const std::wstring& b) {
    auto na = normalize_for_match(a);
    auto nb = normalize_for_match(b);
    return !na.empty() && na == nb;
}

std::wstring command_line_quote(const std::wstring& value) {
    std::wstring out = L"\"";
    unsigned backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            backslashes++;
        } else if (ch == L'\"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'\"');
    return out;
}

std::wstring component_dir() {
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleFileNameW(core_api::get_my_instance(), path, _countof(path));
    return fs::path(path).parent_path().wstring();
}

fs::path downloader_path() {
    return fs::path(component_dir()) / L"downloader" / L"LrcDownloader.exe";
}

std::wstring source_display_name(const std::wstring& key) {
    if (_wcsicmp(key.c_str(), L"lrclib") == 0) return L"LRCLIB";
    if (_wcsicmp(key.c_str(), L"qq1") == 0) return L"QQ音乐1号源";
    if (_wcsicmp(key.c_str(), L"qq2") == 0) return L"QQ音乐2号源";
    if (_wcsicmp(key.c_str(), L"netease") == 0 || _wcsicmp(key.c_str(), L"163") == 0) return L"网易云音乐";
    return key;
}

std::vector<std::wstring> split_tab_line(const std::wstring& line) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= line.size()) {
        size_t end = line.find(L'\t', start);
        if (end == std::wstring::npos) end = line.size();
        parts.push_back(line.substr(start, end - start));
        if (end == line.size()) break;
        start = end + 1;
    }
    return parts;
}

bool parse_nonnegative_index(const std::wstring& value, int& result) {
    const std::wstring trimmed = trim_text(value);
    if (trimmed.empty()) return false;

    wchar_t* end = nullptr;
    const long parsed = wcstol(trimmed.c_str(), &end, 10);
    if (end == trimmed.c_str() || *end != L'\0' || parsed < 0 ||
        parsed > static_cast<long>(INT_MAX)) {
        return false;
    }

    result = static_cast<int>(parsed);
    return true;
}

std::vector<search_result_item> parse_search_output(const std::string& output) {
    std::vector<search_result_item> items;
    std::wstring text = utf8_to_wide_local(output);
    std::wstringstream ss(text);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line.rfind(L"WARN:", 0) == 0 || line.rfind(L"ERROR:", 0) == 0) continue;
        auto parts = split_tab_line(line);
        if (parts.size() < 3 || trim_text(parts[0]).empty()) continue;
        search_result_item item;
        int explicitIndex = -1;
        const bool hasExplicitIndex = parts.size() >= 4 &&
            parse_nonnegative_index(parts[0], explicitIndex);
        const size_t titlePart = hasExplicitIndex ? 1 : 0;
        const size_t artistPart = hasExplicitIndex ? 2 : 1;
        const size_t sourcePart = hasExplicitIndex ? 3 : 2;
        if (sourcePart >= parts.size()) continue;
        item.candidate_index = hasExplicitIndex
            ? explicitIndex : static_cast<int>(items.size());
        item.title = trim_text(parts[titlePart]);
        item.artist = trim_text(parts[artistPart]);
        item.source_key = trim_text(parts[sourcePart]);
        item.source_name = source_display_name(item.source_key);
        if (item.title.empty()) continue;
        items.push_back(item);
    }
    return items;
}

std::wstring display_text(const search_result_item& item) {
    if (item.placeholder) return L"\u6CA1\u6709\u641C\u7D22\u5230\u7ED3\u679C";
    std::wstring text = item.title;
    if (!item.artist.empty()) {
        text += L"\uFF0C";
        text += item.artist;
    }
    text += L"\uFF0C\u6765\u6E90\uFF1A";
    text += item.source_name.empty() ? item.source_key : item.source_name;
    return text;
}

void refresh_result_list(const std::vector<search_result_item>& items, bool select_first) {
    const size_t previousCount = g_items.size();
    g_items = items;
    if (!g_list) return;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& item : g_items) {
        std::wstring text = display_text(item);
        const LRESULT result = SendMessageW(
            g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (result == LB_ERR || result == LB_ERRSPACE) {
            speaklyrics_log_error(
                L"手动搜索：结果列表添加项目失败，项目序号=%llu，错误码=%lld。",
                static_cast<unsigned long long>(&item - g_items.data()),
                static_cast<long long>(result));
        }
    }
    if (select_first && !g_items.empty() && !g_items.front().placeholder) {
        SendMessageW(g_list, LB_SETCURSEL, 0, 0);
    } else {
        SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
    speaklyrics_log_info(
        L"手动搜索：结果列表已刷新，旧条目=%llu，新条目=%llu，默认选择=%s。",
        static_cast<unsigned long long>(previousCount),
        static_cast<unsigned long long>(g_items.size()),
        (select_first && !g_items.empty() && !g_items.front().placeholder) ? L"第1项" : L"无");
}

void set_searching(bool searching) {
    g_searching = searching;
    if (g_search_button) EnableWindow(g_search_button, searching ? FALSE : TRUE);
    if (g_progress) {
        ShowWindow(g_progress, searching ? SW_SHOW : SW_HIDE);
        SendMessageW(g_progress, PBM_SETMARQUEE, searching ? TRUE : FALSE, searching ? 30 : 0);
    }
}

void clear_manual_candidate_cache() {
    if (g_manual_candidate_cache_path.empty()) return;

    std::error_code error;
    const fs::path path(g_manual_candidate_cache_path);
    fs::remove(path, error);
    std::error_code existsError;
    if (error && fs::exists(path, existsError)) {
        speaklyrics_log_warning(L"手动搜索：清理旧候选缓存失败：%s，错误码：%lu。",
            speaklyrics_log_path(g_manual_candidate_cache_path.c_str()).c_str(),
            static_cast<unsigned long>(error.value()));
    }
    g_manual_candidate_cache_path.clear();
}

std::wstring create_manual_candidate_cache() {
    std::error_code error;
    const fs::path folder = fs::temp_directory_path(error);
    if (error || folder.empty()) {
        speaklyrics_log_error(L"手动搜索：无法获取系统临时目录，不能固定搜索候选。错误码：%lu。",
            static_cast<unsigned long>(error.value()));
        return std::wstring();
    }

    const std::wstring fileName =
        L"foo_speaklyrics-manual-candidates-" +
        std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L"-" +
        std::to_wstring(static_cast<unsigned long long>(++g_manual_candidate_cache_id)) +
        L".json";
    return (folder / fileName).wstring();
}

void bind_search_context(std::vector<search_result_item>& items,
    const std::wstring& cachePath, const std::wstring& title,
    const std::wstring& artist, const std::wstring& album,
    int durationSeconds, const std::wstring& sources, bool titleOnly) {
    for (auto& item : items) {
        item.candidate_cache_path = cachePath;
        item.query_title = title;
        item.query_artist = artist;
        item.query_album = album;
        item.query_duration_seconds = durationSeconds;
        item.query_sources = sources;
        item.query_title_only = titleOnly;
    }
}

void cancel_search_background_tasks() {
    if (g_search_task) {
        g_search_task->cancel();
        g_search_task.reset();
    }
    if (g_download_task) {
        g_download_task->cancel();
        g_download_task.reset();
    }
    ++g_window_generation;
    ++g_search_request_id;
    ++g_download_request_id;
    g_searching = false;
    clear_manual_candidate_cache();
}

bool has_enabled_sources() {
    std::wstring sources = cfg_to_wide_local(cfg_lyric_sources);
    return !trim_text(sources).empty();
}

std::wstring output_folder_for_download(bool permanent) {
    std::wstring permanentFolder = trim_text(cfg_to_wide_local(cfg_lrc_folder));
    bool toLrcFolder = permanent || !permanentFolder.empty();
    return expand_environment_path(toLrcFolder ? permanentFolder : cfg_to_wide_local(cfg_temp_lrc_folder));
}

bool is_temporary_download(bool permanent) {
    return !permanent && trim_text(cfg_to_wide_local(cfg_lrc_folder)).empty();
}

bool download_item_to_folder(const search_result_item& item, const std::wstring& folder,
    bool temporary, const std::wstring& source, const std::wstring& manifestPath,
    const fs::path& exe, foobar2000_io::abort_callback& aborter, std::wstring& error) {
    if (item.placeholder) return false;
    if (trim_text(folder).empty()) {
        error = L"\u6CA1\u6709\u8BBE\u7F6ELRC\u6B4C\u8BCD\u76EE\u5F55";
        speaklyrics_log_error(L"手动搜索下载：没有设置输出目录。");
        return false;
    }

    aborter.check();
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec || !fs::is_directory(folder, ec)) {
        error = L"\u65E0\u6CD5\u4F7F\u7528LRC\u6B4C\u8BCD\u76EE\u5F55";
        speaklyrics_log_error(L"手动搜索下载：输出目录不可用：%s。", speaklyrics_log_path(folder.c_str()).c_str());
        return false;
    }
    ec.clear();
    if (!fs::exists(exe, ec) || ec) {
        error = L"\u627E\u4E0D\u5230\u6B4C\u8BCD\u4E0B\u8F7D\u5668";
        speaklyrics_log_error(L"手动搜索下载：找不到歌词下载器：%s。", speaklyrics_log_path(exe.c_str()).c_str());
        return false;
    }

    const std::wstring queryTitle = item.query_title.empty() ? item.title : item.query_title;
    const std::wstring queryArtist = item.query_artist.empty() ? item.artist : item.query_artist;
    const std::wstring queryAlbum = item.query_album;
    const std::wstring querySources = item.query_sources.empty() ? source : item.query_sources;
    std::wstring cmd = command_line_quote(exe.wstring()) +
        L" --title " + command_line_quote(queryTitle) +
        L" --artist " + command_line_quote(queryArtist) +
        L" --album " + command_line_quote(queryAlbum) +
        L" --duration " + std::to_wstring(item.query_duration_seconds) +
        L" --sources " + command_line_quote(querySources) +
        L" --out " + command_line_quote(folder) +
        L" --search-only";
    if (item.query_title_only) cmd += L" --title-only";
    if (item.candidate_index >= 0 && !item.candidate_cache_path.empty()) {
        cmd += L" --candidate-index " + std::to_wstring(item.candidate_index) +
            L" --candidate-cache " + command_line_quote(item.candidate_cache_path) +
            L" --candidate-exact";
    }
    if (temporary && !manifestPath.empty()) {
        cmd += L" --manifest " + command_line_quote(manifestPath);
    }

    const speaklyrics_process_result process = run_process_capture_stdout(
        cmd, exe.parent_path(), aborter, 60000, 1024 * 1024);
    if (process.status != speaklyrics_process_status::completed || process.exit_code != 0) {
        error = L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25";
        speaklyrics_log_error(
            L"手动搜索下载：进程失败，状态=%s，退出码：%lu，错误码：%lu，标题：%s，艺术家：%s。",
            speaklyrics_process_status_name(process.status), process.exit_code,
            process.error_code, speaklyrics_log_private_text(item.title.c_str()).c_str(), speaklyrics_log_private_text(item.artist.c_str()).c_str());
        return false;
    }
    speaklyrics_log_info(L"手动搜索下载：下载成功，标题：%s，艺术家：%s，目录：%s。",
        speaklyrics_log_private_text(item.title.c_str()).c_str(), speaklyrics_log_private_text(item.artist.c_str()).c_str(), speaklyrics_log_path(folder.c_str()).c_str());
    return true;
}

bool should_auto_download(const search_result_item& item, const std::wstring& searchTitle,
    const std::wstring& searchArtist, const std::wstring& currentArtist) {
    if (item.placeholder) return false;
    if (!same_match(item.title, searchTitle)) return false;
    if (!trim_text(searchArtist).empty()) return same_match(item.artist, searchArtist);
    if (!trim_text(currentArtist).empty()) return same_match(item.artist, currentArtist);
    return false;
}

bool start_download(size_t index, bool permanent) {
    if (index >= g_items.size() || g_items[index].placeholder) {
        speaklyrics_log_warning(L"手动搜索下载：没有可下载的候选，项目序号=%llu。",
            static_cast<unsigned long long>(index));
        return false;
    }
    const search_result_item item = g_items[index];
    const std::wstring folder = output_folder_for_download(permanent);
    const bool temporary = is_temporary_download(permanent);
    const std::wstring source = item.source_key.empty()
        ? cfg_to_wide_local(cfg_lyric_sources) : item.source_key;
    const std::wstring manifestPath = temporary ? temp_lrc_manifest_path() : L"";
    const fs::path exe = downloader_path();
    const HWND window = g_window;
    const uint64_t windowGeneration = g_window_generation;

    if (g_download_task) {
        speaklyrics_log_warning(
            L"手动搜索下载：已有下载任务正在进行，忽略重复请求；候选序号=%d，标题：%s，艺术家：%s。",
            item.candidate_index, speaklyrics_log_private_text(item.title.c_str()).c_str(), speaklyrics_log_private_text(item.artist.c_str()).c_str());
        speech_queue_speak(L"正在下载上一条歌词，请稍候", true);
        return false;
    }
    auto task = speaklyrics_start_background_task(L"manual lyric download");
    if (!task) {
        speaklyrics_log_warning(L"手动搜索下载：组件正在退出，未启动下载任务。");
        return false;
    }
    const uint64_t request = ++g_download_request_id;
    g_download_task = task;
    speaklyrics_log_info(
        L"手动搜索下载：提交候选，候选序号=%d，标题：%s，艺术家：%s，来源：%s，缓存：%s。",
        item.candidate_index, speaklyrics_log_private_text(item.title.c_str()).c_str(), speaklyrics_log_private_text(item.artist.c_str()).c_str(),
        item.source_key.c_str(), speaklyrics_log_path(item.candidate_cache_path.c_str()).c_str());
    speaklyrics_run_background_task(task,
        [task, item, folder, temporary, source, manifestPath, exe, window,
            windowGeneration, request](speaklyrics_background_task& background) {
        std::wstring error;
        bool ok = false;
        try {
            ok = download_item_to_folder(item, folder, temporary, source, manifestPath,
                exe, background.aborter(), error);
        } catch (const foobar2000_io::exception_aborted&) {
            return;
        } catch (...) {
            error = L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25";
            speaklyrics_log_error(L"手动搜索下载：下载任务发生未知异常。");
        }
        background.post_to_main_thread([task, ok, error, window, windowGeneration, request]() {
            if (g_window != window || !IsWindow(window) ||
                g_window_generation != windowGeneration ||
                g_download_request_id != request || g_download_task.get() != task.get()) {
                return;
            }
            g_download_task.reset();
            if (ok) {
                reload_current_lyrics();
            } else {
                pfc::string8 msg = wide_to_pfc_utf8(
                    error.empty() ? L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25" : error);
                popup_message::g_show(msg.get_ptr(), "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
                speech_queue_speak(error.empty() ? L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25" : error.c_str(), true);
            }
        });
    });
    return true;
}

void auto_fill_current_playing() {
    current_track_search_info info = get_current_track_search_info();
    if (g_title_edit) SetWindowTextW(g_title_edit, info.title.c_str());
    if (g_artist_edit) SetWindowTextW(g_artist_edit, info.artist.c_str());
}

void start_search() {
    if (g_searching) return;
    if (!has_enabled_sources()) {
        speaklyrics_log_warning(L"手动搜索：没有启用歌词下载来源。");
        popup_message::g_show("\xE6\xB2\xA1\xE6\x9C\x89\xE5\x90\xAF\xE7\x94\xA8\xE6\xAD\x8C\xE8\xAF\x8D\xE4\xB8\x8B\xE8\xBD\xBD\xE6\x9D\xA5\xE6\xBA\x90", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        speech_queue_speak(L"\u6CA1\u6709\u542F\u7528\u6B4C\u8BCD\u4E0B\u8F7D\u6765\u6E90", true);
        return;
    }
    std::wstring title = trim_text(get_window_text(g_title_edit));
    std::wstring artist = trim_text(get_window_text(g_artist_edit));
    if (title.empty()) {
        speaklyrics_log_warning(L"手动搜索：标题为空。");
        popup_message::g_show("\xE8\xAF\xB7\xE5\x85\x88\xE5\xA1\xAB\xE5\x86\x99\xE6\xA0\x87\xE9\xA2\x98", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        if (g_title_edit) SetFocus(g_title_edit);
        return;
    }
    current_track_search_info current = get_current_track_search_info();
    std::wstring fallbackArtist = artist.empty() ? current.artist : artist;
    std::wstring sources = cfg_to_wide_local(cfg_lyric_sources);
    fs::path exe = downloader_path();
    std::error_code exeError;
    if (!fs::is_regular_file(exe, exeError) || exeError) {
        speaklyrics_log_error(L"手动搜索：找不到歌词下载器：%s。", speaklyrics_log_path(exe.c_str()).c_str());
        popup_message::g_show("\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0\xE6\xAD\x8C\xE8\xAF\x8D\xE4\xB8\x8B\xE8\xBD\xBD\xE5\x99\xA8", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        return;
    }
    clear_manual_candidate_cache();
    const std::wstring candidateCachePath = create_manual_candidate_cache();
    if (candidateCachePath.empty()) {
        popup_message::g_show("\xE6\x97\xA0\xE6\xB3\x95\xE5\x88\x9B\xE5\xBB\xBA\xE6\x90\x9C\xE7\xB4\xA2\xE5\x80\x99\xE9\x80\x89\xE7\xBC\x93\xE5\xAD\x98", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        return;
    }
    g_manual_candidate_cache_path = candidateCachePath;
    speaklyrics_log_info(L"手动搜索：开始搜索，标题：%s，艺术家：%s，来源：%s。", speaklyrics_log_private_text(title.c_str()).c_str(), speaklyrics_log_private_text(fallbackArtist.c_str()).c_str(), sources.c_str());
    set_searching(true);
    std::wstring command = command_line_quote(exe.wstring()) +
        L" --list --title " + command_line_quote(title) +
        L" --artist " + command_line_quote(fallbackArtist) +
        L" --album " + command_line_quote(current.album) +
        L" --duration " + std::to_wstring(current.duration_seconds) +
        L" --sources " + command_line_quote(sources) +
        L" --candidate-cache " + command_line_quote(candidateCachePath);

    const std::wstring outputFolder = output_folder_for_download(false);
    const bool temporaryDownload = is_temporary_download(false);
    const std::wstring manifestPath = temporaryDownload ? temp_lrc_manifest_path() : L"";
    const HWND window = g_window;
    const HWND focusBeforeSearch = GetFocus();
    const bool focusWasInsideSearchWindow = window_contains_focus(window, focusBeforeSearch);
    const uint64_t windowGeneration = g_window_generation;
    const uint64_t request = ++g_search_request_id;
    const std::wstring searchCandidateCachePath = candidateCachePath;
    auto task = speaklyrics_start_background_task(L"manual lyric search");
    if (!task) {
        set_searching(false);
        speaklyrics_log_warning(L"Background task skipped during shutdown.");
        return;
    }
    g_search_task = task;
    speaklyrics_run_background_task(task,
        [task, command, exe, title, artist, currentArtist = current.artist,
            fallbackArtist, currentAlbum = current.album,
            currentDuration = current.duration_seconds, sources, outputFolder,
            temporaryDownload, manifestPath, searchCandidateCachePath, window,
            focusWasInsideSearchWindow, windowGeneration, request](speaklyrics_background_task& background) {
        const speaklyrics_process_result process = run_process_capture_stdout(
            command, exe.parent_path(), background.aborter(), 30000, 1024 * 1024);
        std::vector<search_result_item> items;
        bool autoDownloaded = false;
        if (process.status == speaklyrics_process_status::completed && process.exit_code == 0) {
            items = parse_search_output(process.output);
            bind_search_context(items, searchCandidateCachePath, title, fallbackArtist,
                currentAlbum, currentDuration, sources, false);
        }
        if (items.empty()) {
            search_result_item empty;
            empty.placeholder = true;
            items.push_back(empty);
        } else if (should_auto_download(items.front(), title, artist, currentArtist) &&
            !trim_text(outputFolder).empty()) {
            const std::wstring source = items.front().source_key.empty()
                ? sources : items.front().source_key;
            std::wstring downloadError;
            autoDownloaded = download_item_to_folder(items.front(), outputFolder,
                temporaryDownload, source, manifestPath, exe, background.aborter(), downloadError);
        }

        const speaklyrics_process_status status = process.status;
        const DWORD processExitCode = process.exit_code;
        const DWORD processErrorCode = process.error_code;
        background.post_to_main_thread([task, window, focusWasInsideSearchWindow,
            windowGeneration, request,
            status, processExitCode, processErrorCode, items = std::move(items),
            autoDownloaded]() mutable {
            if (g_window != window || !IsWindow(window) ||
                g_window_generation != windowGeneration ||
                g_search_request_id != request || g_search_task.get() != task.get()) {
                return;
            }
            g_search_task.reset();
            set_searching(false);
            if (status != speaklyrics_process_status::completed || processExitCode != 0) {
                speaklyrics_log_warning(
                    L"手动搜索：进程未完成，状态=%s，退出码：%lu，错误码：%lu。",
                    speaklyrics_process_status_name(status), processExitCode, processErrorCode);
            }
            const HWND focusBeforeRefresh = GetFocus();
            const bool focusStillInsideSearchWindow =
                window_contains_focus(window, focusBeforeRefresh);
            const bool shouldFocusResults =
                GetForegroundWindow() == window &&
                focusWasInsideSearchWindow && focusStillInsideSearchWindow;
            refresh_result_list(items, true);
            if (autoDownloaded) reload_current_lyrics();
            if (shouldFocusResults && g_list) {
                SetFocus(g_list);
            } else {
                speaklyrics_log_info(
                    L"手动搜索：搜索完成时不移动焦点，窗口前台=%s，原焦点属于搜索窗口=%s，当前焦点属于搜索窗口=%s。",
                    GetForegroundWindow() == window ? L"是" : L"否",
                    focusWasInsideSearchWindow ? L"是" : L"否",
                    focusStillInsideSearchWindow ? L"是" : L"否");
            }
        });
    });
}


void activate_existing_window() {
    if (!g_window || !IsWindow(g_window)) return;
    if (IsIconic(g_window)) ShowWindow(g_window, SW_RESTORE);
    ShowWindow(g_window, SW_SHOW);
    BringWindowToTop(g_window);
    SetForegroundWindow(g_window);
    HWND focus = GetFocus();
    if (!focus || !IsChild(g_window, focus)) SetFocus(g_auto_button ? g_auto_button : g_window);
}

bool download_selected(bool permanent) {
    int sel = g_list ? static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0 || sel >= static_cast<int>(g_items.size())) {
        speaklyrics_log_warning(L"手动搜索下载：当前没有选中的结果，列表选择=%d，条目数=%llu。",
            sel, static_cast<unsigned long long>(g_items.size()));
        return false;
    }
    speaklyrics_log_info(L"手动搜索下载：用户请求下载列表第%d项，候选序号=%d。",
        sel + 1, g_items[static_cast<size_t>(sel)].candidate_index);
    return start_download(static_cast<size_t>(sel), permanent);
}

void show_context_menu(POINT pt) {
    if (!g_list) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"\u4E0B\u8F7D\u5230\u8BBE\u7F6E\u7684LRC\u6B4C\u8BCD\u76EE\u5F55");
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_window, nullptr);
    DestroyMenu(menu);
    if (cmd == 1001) download_selected(true);
}

LRESULT CALLBACK list_subclass_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_GETDLGCODE) {
        LRESULT result = DefSubclassProc(wnd, msg, wp, lp);
        const MSG* keyMessage = reinterpret_cast<const MSG*>(lp);
        if (wp == VK_RETURN ||
            (keyMessage && keyMessage->message == WM_KEYDOWN &&
                keyMessage->wParam == VK_RETURN)) {
            result |= DLGC_WANTMESSAGE;
        }
        return result;
    }
    if (msg == WM_KEYDOWN) {
        if (wp == VK_SPACE) {
            if (SendMessageW(g_list, LB_GETCURSEL, 0, 0) == LB_ERR && !g_items.empty()) SendMessageW(g_list, LB_SETCURSEL, 0, 0);
            return 0;
        }
        if (wp == VK_RETURN) {
            download_selected(false);
            return 0;
        }
        if (wp == VK_APPS || (wp == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000))) {
            RECT rc = {};
            int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
            if (sel >= 0) SendMessageW(g_list, LB_GETITEMRECT, sel, reinterpret_cast<LPARAM>(&rc));
            POINT pt = { rc.left + 12, rc.top + 12 };
            ClientToScreen(g_list, &pt);
            show_context_menu(pt);
            return 0;
        }
    }
    if (msg == WM_CHAR && wp == L'\r') return 0;
    return DefSubclassProc(wnd, msg, wp, lp);
}

void reset_dialog_handles(HWND wnd) {
    if (wnd == g_window) {
        cancel_search_background_tasks();
        if (g_list && IsWindow(g_list)) RemoveWindowSubclass(g_list, list_subclass_proc, 1);
        g_window = g_auto_button = g_title_edit = g_artist_edit = g_search_button = g_title_label = g_artist_label = g_progress = g_list = g_close_button = nullptr;
        g_items.clear();
        g_searching = false;
    }
}

INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        ++g_window_generation;
        g_window = wnd;
        g_auto_button = GetDlgItem(wnd, IDC_SEARCH_AUTO_FILL);
        g_title_edit = GetDlgItem(wnd, IDC_SEARCH_TITLE);
        g_artist_edit = GetDlgItem(wnd, IDC_SEARCH_ARTIST);
        g_search_button = GetDlgItem(wnd, IDC_SEARCH_BUTTON);
        g_title_label = GetDlgItem(wnd, IDC_STATIC_SEARCH_TITLE);
        g_artist_label = GetDlgItem(wnd, IDC_STATIC_SEARCH_ARTIST);
        g_progress = GetDlgItem(wnd, IDC_SEARCH_PROGRESS);
        g_list = GetDlgItem(wnd, IDC_SEARCH_RESULTS);
        g_close_button = GetDlgItem(wnd, IDCANCEL);
        SetWindowTextW(wnd, L"\u641C\u7D22lrc\u6B4C\u8BCD");
        SetWindowTextW(g_auto_button, L"\u81EA\u52A8\u586B\u5165\u5F53\u524D\u64AD\u653E\u4FE1\u606F(&A)");
        SetWindowTextW(g_title_label, L"\u6807\u9898");
        SetWindowTextW(g_artist_label, L"\u827A\u672F\u5BB6\uFF0C\u53EF\u9009");
        SetWindowTextW(g_search_button, L"\u641C\u7D22(&S)");
        SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_SEARCH_RESULTS), L"\u7ED3\u679C");
        SetWindowTextW(g_close_button, L"\u5173\u95ED(&C)");
        set_accessible_name(g_auto_button, L"\u81EA\u52A8\u586B\u5165\u5F53\u524D\u64AD\u653E\u4FE1\u606F");
        set_accessible_name(g_title_edit, L"\u6807\u9898");
        set_accessible_name(g_artist_edit, L"\u827A\u672F\u5BB6\uFF0C\u53EF\u9009");
        set_accessible_name(g_search_button, L"\u641C\u7D22");
        set_accessible_name(g_progress, L"\u641C\u7D22\u8FDB\u5EA6");
        set_accessible_name(g_list, L"\u7ED3\u679C");
        set_accessible_name(g_close_button, L"\u5173\u95ED");
        if (g_progress) ShowWindow(g_progress, SW_HIDE);
        if (g_list) SetWindowSubclass(g_list, list_subclass_proc, 1, 0);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SEARCH_AUTO_FILL:
            if (HIWORD(wp) == BN_CLICKED) {
                auto_fill_current_playing();
                if (g_title_edit) SetFocus(g_title_edit);
                return TRUE;
            }
            break;
        case IDC_SEARCH_BUTTON:
            if (HIWORD(wp) == BN_CLICKED) {
                start_search();
                return TRUE;
            }
            break;
        case IDC_SEARCH_RESULTS:
            if (HIWORD(wp) == LBN_DBLCLK) {
                download_selected(false);
                return TRUE;
            }
            break;
        case IDCANCEL:
            EndDialog(wnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == g_list) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (pt.x == -1 && pt.y == -1) { RECT rc = {}; GetWindowRect(g_list, &rc); pt = { rc.left + 20, rc.top + 20 }; }
            show_context_menu(pt);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(wnd, IDCANCEL);
        return TRUE;
    case WM_NCDESTROY:
        reset_dialog_handles(wnd);
        return TRUE;
    }
    return FALSE;
}

}

void show_lyrics_search_window(HWND parent) {
    if (g_window && IsWindow(g_window)) {
        activate_existing_window();
        return;
    }
    DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_SEARCH_LRC), parent, dialog_proc, 0);
}

void cancel_lyrics_search_background_tasks() {
    cancel_search_background_tasks();
}

