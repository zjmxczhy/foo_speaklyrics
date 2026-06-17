#include "stdafx.h"

#include "lyrics_search_window.h"
#include "config.h"
#include "playback.h"
#include "resource.h"
#include "speech_engine.h"

#include <cwctype>
#include <sstream>
#include <thread>
#include <windowsx.h>

namespace {

constexpr UINT WM_SEARCH_DONE = WM_APP + 0x451;
constexpr UINT WM_DOWNLOAD_DONE = WM_APP + 0x452;

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
std::wstring g_last_search_title;
std::wstring g_last_search_artist;
std::wstring g_last_current_artist;

struct search_result_item {
    std::wstring title;
    std::wstring artist;
    std::wstring source_key;
    std::wstring source_name;
    bool placeholder = false;
};

struct search_done_payload {
    std::vector<search_result_item> items;
    bool auto_downloaded = false;
};

struct download_done_payload {
    bool ok = false;
    std::wstring error;
};

std::vector<search_result_item> g_items;

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

std::wstring cfg_to_wide_local(cfg_var_modern::cfg_string& s) {
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
    BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
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
        item.title = trim_text(parts[0]);
        item.artist = trim_text(parts[1]);
        item.source_key = trim_text(parts[2]);
        item.source_name = source_display_name(item.source_key);
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

void refresh_result_list(const std::vector<search_result_item>& items) {
    g_items = items;
    if (!g_list) return;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& item : g_items) {
        std::wstring text = display_text(item);
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

void set_searching(bool searching) {
    g_searching = searching;
    if (g_search_button) EnableWindow(g_search_button, searching ? FALSE : TRUE);
    if (g_progress) {
        ShowWindow(g_progress, searching ? SW_SHOW : SW_HIDE);
        SendMessageW(g_progress, PBM_SETMARQUEE, searching ? TRUE : FALSE, searching ? 30 : 0);
    }
}

bool has_enabled_sources() {
    std::wstring sources = cfg_to_wide_local(cfg_lyric_sources);
    return !trim_text(sources).empty();
}

std::wstring output_folder_for_download(bool permanent) {
    return expand_environment_path(permanent ? cfg_to_wide_local(cfg_lrc_folder) : cfg_to_wide_local(cfg_temp_lrc_folder));
}

bool download_item_to_folder(const search_result_item& item, const std::wstring& folder, std::wstring& error) {
    if (item.placeholder) return false;
    if (trim_text(folder).empty()) {
        error = L"\u6CA1\u6709\u8BBE\u7F6ELRC\u6B4C\u8BCD\u76EE\u5F55";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    fs::path exe = downloader_path();
    if (!fs::exists(exe, ec)) {
        error = L"\u627E\u4E0D\u5230\u6B4C\u8BCD\u4E0B\u8F7D\u5668";
        return false;
    }
    std::wstring source = item.source_key.empty() ? cfg_to_wide_local(cfg_lyric_sources) : item.source_key;
    std::wstring cmd = command_line_quote(exe.wstring()) +
        L" --title " + command_line_quote(item.title) +
        L" --artist " + command_line_quote(item.artist) +
        L" --sources " + command_line_quote(source) +
        L" --out " + command_line_quote(folder) +
        L" --search-only";
    std::string output;
    DWORD code = 3;
    bool ok = run_process_capture_stdout(cmd, exe.parent_path(), output, code);
    if (!ok || code != 0) {
        error = L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25";
        return false;
    }
    return true;
}

bool should_auto_download(const search_result_item& item) {
    if (item.placeholder) return false;
    if (!same_match(item.title, g_last_search_title)) return false;
    if (!trim_text(g_last_search_artist).empty()) return same_match(item.artist, g_last_search_artist);
    if (!trim_text(g_last_current_artist).empty()) return same_match(item.artist, g_last_current_artist);
    return false;
}

void start_download(size_t index, bool permanent) {
    if (index >= g_items.size() || g_items[index].placeholder) return;
    search_result_item item = g_items[index];
    std::wstring folder = output_folder_for_download(permanent);
    std::thread([item, folder]() {
        auto payload = new download_done_payload();
        payload->ok = download_item_to_folder(item, folder, payload->error);
        if (g_window && IsWindow(g_window)) PostMessageW(g_window, WM_DOWNLOAD_DONE, 0, reinterpret_cast<LPARAM>(payload));
        else delete payload;
    }).detach();
}

void auto_fill_current_playing() {
    current_track_search_info info = get_current_track_search_info();
    if (g_title_edit) SetWindowTextW(g_title_edit, info.title.c_str());
    if (g_artist_edit) SetWindowTextW(g_artist_edit, info.artist.c_str());
}

void start_search() {
    if (g_searching) return;
    if (!has_enabled_sources()) {
        popup_message::g_show("\xE6\xB2\xA1\xE6\x9C\x89\xE5\x90\xAF\xE7\x94\xA8\xE6\xAD\x8C\xE8\xAF\x8D\xE4\xB8\x8B\xE8\xBD\xBD\xE6\x9D\xA5\xE6\xBA\x90", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        speech_queue_speak(L"\u6CA1\u6709\u542F\u7528\u6B4C\u8BCD\u4E0B\u8F7D\u6765\u6E90", true);
        return;
    }
    std::wstring title = trim_text(get_window_text(g_title_edit));
    std::wstring artist = trim_text(get_window_text(g_artist_edit));
    if (title.empty()) {
        popup_message::g_show("\xE8\xAF\xB7\xE5\x85\x88\xE5\xA1\xAB\xE5\x86\x99\xE6\xA0\x87\xE9\xA2\x98", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        if (g_title_edit) SetFocus(g_title_edit);
        return;
    }
    current_track_search_info current = get_current_track_search_info();
    g_last_search_title = title;
    g_last_search_artist = artist;
    g_last_current_artist = current.artist;
    std::wstring fallbackArtist = artist.empty() ? current.artist : artist;
    std::wstring sources = cfg_to_wide_local(cfg_lyric_sources);
    fs::path exe = downloader_path();
    if (!fs::exists(exe)) {
        popup_message::g_show("\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0\xE6\xAD\x8C\xE8\xAF\x8D\xE4\xB8\x8B\xE8\xBD\xBD\xE5\x99\xA8", "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
        return;
    }
    set_searching(true);
    std::wstring command = command_line_quote(exe.wstring()) +
        L" --list --title " + command_line_quote(title) +
        L" --artist " + command_line_quote(fallbackArtist) +
        L" --album " + command_line_quote(current.album) +
        L" --duration " + std::to_wstring(current.duration_seconds) +
        L" --sources " + command_line_quote(sources);

    std::thread([command, exe]() {
        std::string output;
        DWORD code = 3;
        run_process_capture_stdout(command, exe.parent_path(), output, code);
        auto payload = new search_done_payload();
        if (code == 0) payload->items = parse_search_output(output);
        if (payload->items.empty()) {
            search_result_item empty;
            empty.placeholder = true;
            payload->items.push_back(empty);
        } else if (should_auto_download(payload->items.front())) {
            std::wstring error;
            std::wstring tempFolder = expand_environment_path(cfg_to_wide_local(cfg_temp_lrc_folder));
            if (!trim_text(tempFolder).empty() && download_item_to_folder(payload->items.front(), tempFolder, error)) {
                payload->auto_downloaded = true;
            }
        }
        if (g_window && IsWindow(g_window)) PostMessageW(g_window, WM_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(payload));
        else delete payload;
    }).detach();
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

void download_selected(bool permanent) {
    int sel = g_list ? static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0)) : -1;
    if (sel < 0 || sel >= static_cast<int>(g_items.size())) return;
    start_download(static_cast<size_t>(sel), permanent);
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
    return DefSubclassProc(wnd, msg, wp, lp);
}

void reset_dialog_handles(HWND wnd) {
    if (wnd == g_window) {
        if (g_list && IsWindow(g_list)) RemoveWindowSubclass(g_list, list_subclass_proc, 1);
        g_window = g_auto_button = g_title_edit = g_artist_edit = g_search_button = g_title_label = g_artist_label = g_progress = g_list = g_close_button = nullptr;
        g_items.clear();
        g_searching = false;
    }
}

INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
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
    case WM_SEARCH_DONE: {
        auto payload = reinterpret_cast<search_done_payload*>(lp);
        set_searching(false);
        if (payload) {
            refresh_result_list(payload->items);
            if (payload->auto_downloaded) reload_current_lyrics();
            delete payload;
        }
        if (g_list) SetFocus(g_list);
        return TRUE;
    }
    case WM_DOWNLOAD_DONE: {
        auto payload = reinterpret_cast<download_done_payload*>(lp);
        if (payload) {
            if (payload->ok) reload_current_lyrics();
            else {
                pfc::string8 msg = wide_to_pfc_utf8(payload->error.empty() ? L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25" : payload->error);
                popup_message::g_show(msg.get_ptr(), "\xE6\x90\x9C\xE7\xB4\xA2lrc\xE6\xAD\x8C\xE8\xAF\x8D");
                speech_queue_speak(payload->error.empty() ? L"\u6B4C\u8BCD\u4E0B\u8F7D\u5931\u8D25" : payload->error.c_str(), true);
            }
            delete payload;
        }
        return TRUE;
    }
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

