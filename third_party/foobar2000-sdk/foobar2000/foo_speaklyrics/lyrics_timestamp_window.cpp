#include "stdafx.h"

#include "lyrics_timestamp_window.h"

#include "lrc_parser.h"
#include "playback.h"
#include "resource.h"
#include "speech_engine.h"
#include "speaklyrics_log.h"

#include <cwctype>
#include <windowsx.h>

namespace {

HWND g_window = nullptr;
HWND g_time_edit = nullptr;
HWND g_refresh_time_button = nullptr;
HWND g_text_edit = nullptr;
HWND g_encoding_combo = nullptr;
HWND g_preview_edit = nullptr;
HWND g_save_button = nullptr;
HWND g_save_as_button = nullptr;
HWND g_open_button = nullptr;
HWND g_close_button = nullptr;
std::wstring g_current_path;
bool g_dirty = false;
bool g_internal_text_change = false;

void set_accessible_name(HWND wnd, const wchar_t* name) {
    if (!wnd) return;
    SetPropW(wnd, L"Name", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(name)));
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, wnd, OBJID_CLIENT, CHILDID_SELF);
}

std::wstring get_window_text(HWND wnd) {
    if (!wnd) return std::wstring();
    int len = GetWindowTextLengthW(wnd);
    std::wstring out(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(wnd, out.data(), len + 1);
    out.resize(static_cast<size_t>(len));
    return out;
}

void set_window_text_silent(HWND wnd, const std::wstring& text) {
    g_internal_text_change = true;
    SetWindowTextW(wnd, text.c_str());
    g_internal_text_change = false;
}

std::wstring trim_text(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}

pfc::string8 wide_to_utf8(const std::wstring& text) {
    return pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr();
}

void show_error(const wchar_t* message) {
    popup_message::g_show(wide_to_utf8(message).get_ptr(), "\xE6\xB7\xBB\xE5\x8A\xA0\xE5\xBD\x93\xE5\x89\x8D\xE6\x97\xB6\xE9\x97\xB4\xE6\xAD\x8C\xE8\xAF\x8D");
}

std::wstring format_lrc_time(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalCentiseconds = static_cast<int>(seconds * 100.0 + 0.5);
    int minutes = totalCentiseconds / 6000;
    int remainder = totalCentiseconds % 6000;
    int secs = remainder / 100;
    int centiseconds = remainder % 100;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"[%02d:%02d.%02d]", minutes, secs, centiseconds);
    return buffer;
}

std::wstring current_playback_lrc_time() {
    static_api_ptr_t<playback_control> playback;
    if (!playback->is_playing()) return L"[00:00.00]";
    return format_lrc_time(playback->playback_get_position());
}

void refresh_time_edit() {
    if (g_time_edit) set_window_text_silent(g_time_edit, current_playback_lrc_time());
}

void refresh_time_edit_and_speak() {
    HWND oldFocus = GetFocus();
    refresh_time_edit();
    std::wstring time = get_window_text(g_time_edit);
    if (!time.empty() && time.front() == L'[' && time.back() == L']') {
        time = time.substr(1, time.size() - 2);
    }
    std::wstring message = L"\u5f53\u524d\u65f6\u95f4";
    if (!time.empty()) {
        message += L" ";
        message += time;
    }
    speech_queue_speak(message.c_str(), true);
    if (oldFocus && IsWindow(oldFocus)) SetFocus(oldFocus);
}

void init_encoding_combo(const char* selected_id) {
    if (!g_encoding_combo) return;
    SendMessageW(g_encoding_combo, CB_RESETCONTENT, 0, 0);
    size_t count = 0;
    const lrc_encoding_info* encodings = lrc_get_encoding_options(count);
    for (size_t i = 0; i < count; ++i) {
        SendMessageW(g_encoding_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(encodings[i].name));
    }
    int index = lrc_find_encoding_index(selected_id && *selected_id ? selected_id : "utf8");
    if (selected_id && _stricmp(selected_id, "auto") == 0) index = lrc_find_encoding_index("utf8");
    SendMessageW(g_encoding_combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

const char* selected_encoding_id() {
    int index = g_encoding_combo ? static_cast<int>(SendMessageW(g_encoding_combo, CB_GETCURSEL, 0, 0)) : -1;
    size_t count = 0;
    const lrc_encoding_info* encodings = lrc_get_encoding_options(count);
    if (index < 0 || index >= static_cast<int>(count)) return "utf8";
    const char* id = encodings[index].id;
    return id && _stricmp(id, "auto") != 0 ? id : "utf8";
}

std::wstring default_lrc_file_name() {
    current_track_search_info info = get_current_track_search_info();
    std::wstring title = trim_text(info.title);
    std::wstring artist = trim_text(info.artist);
    std::wstring name;
    if (!artist.empty() && !title.empty()) name = artist + L" - " + title;
    else if (!title.empty()) name = title;
    else name = L"lyrics";
    for (wchar_t& ch : name) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') ch = L'_';
    }
    name += L".lrc";
    return name;
}

bool browse_open_lrc(HWND parent, std::wstring& path) {
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"LRC\u6b4c\u8bcd (*.lrc)\0*.lrc\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.lpstrTitle = L"\u6253\u5f00LRC\u6b4c\u8bcd";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"lrc";
    if (!GetOpenFileNameW(&ofn)) return false;
    path = file;
    return true;
}

bool browse_save_lrc(HWND parent, std::wstring& path) {
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring initial = path.empty() ? default_lrc_file_name() : fs::path(path).filename().wstring();
    wcsncpy_s(file, initial.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"LRC\u6b4c\u8bcd (*.lrc)\0*.lrc\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = _countof(file);
    ofn.lpstrTitle = L"\u4fdd\u5b58LRC\u6b4c\u8bcd";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"lrc";
    if (!GetSaveFileNameW(&ofn)) return false;
    path = file;
    return true;
}

void set_dirty(bool dirty) {
    g_dirty = dirty;
}

bool save_to_path(const std::wstring& path) {
    if (path.empty()) return false;
    pfc::string8 error;
    if (!lrc_write_text_file(path, get_window_text(g_preview_edit), selected_encoding_id(), error)) {
        std::wstring message = pfc::stringcvt::string_wide_from_utf8(error.get_ptr()).get_ptr();
        if (message.empty()) message = L"\u4fdd\u5b58 LRC \u6587\u4ef6\u5931\u8d25\u3002";
        speaklyrics_log_error(L"添加当前时间歌词：保存失败：%s，文件：%s。", message.c_str(), path.c_str());
        show_error(message.c_str());
        return false;
    }
    speaklyrics_log_info(L"添加当前时间歌词：保存成功：%s。", path.c_str());
    g_current_path = path;
    set_dirty(false);
    return true;
}

bool save_current(bool save_as) {
    std::wstring path = save_as ? std::wstring() : g_current_path;
    if (path.empty() || save_as) {
        path = g_current_path;
        if (!browse_save_lrc(g_window, path)) return false;
    }
    return save_to_path(path);
}

bool confirm_save_if_dirty() {
    if (!g_dirty) return true;
    int result = MessageBoxW(g_window, L"\u5f53\u524d\u6b4c\u8bcd\u6709\u672a\u4fdd\u5b58\u4fee\u6539\uff0c\u662f\u5426\u4fdd\u5b58\uff1f", L"\u6dfb\u52a0\u5f53\u524d\u65f6\u95f4\u6b4c\u8bcd", MB_ICONQUESTION | MB_YESNOCANCEL);
    if (result == IDCANCEL) return false;
    if (result == IDYES) return save_current(false);
    return true;
}

void open_lrc_file() {
    if (!confirm_save_if_dirty()) return;
    std::wstring path;
    if (!browse_open_lrc(g_window, path)) return;

    pfc::string8 error;
    std::string detected;
    std::wstring text = lrc_read_text_file_auto(path, &detected, error);
    if (error.length() > 0) {
        std::wstring message = pfc::stringcvt::string_wide_from_utf8(error.get_ptr()).get_ptr();
        speaklyrics_log_error(L"添加当前时间歌词：打开失败：%s，文件：%s。", message.c_str(), path.c_str());
        show_error(message.empty() ? L"\u6253\u5f00 LRC \u6587\u4ef6\u5931\u8d25\u3002" : message.c_str());
        return;
    }

    set_window_text_silent(g_preview_edit, text);
    g_current_path = path;
    init_encoding_combo(detected.empty() ? "utf8" : detected.c_str());
    set_dirty(false);
    speaklyrics_log_info(L"添加当前时间歌词：打开成功：%s。", path.c_str());
    if (g_preview_edit) SetFocus(g_preview_edit);
}

void append_current_line() {
    std::wstring time = trim_text(get_window_text(g_time_edit));
    std::wstring lyric = trim_text(get_window_text(g_text_edit));
    if (time.empty() || lyric.empty()) return;

    std::wstring line = time + lyric;
    std::wstring preview = get_window_text(g_preview_edit);
    if (!preview.empty() && preview.back() != L'\n') {
        if (preview.back() == L'\r') preview += L"\n";
        else preview += L"\r\n";
    }
    preview += line;
    preview += L"\r\n";
    set_window_text_silent(g_preview_edit, preview);
    set_dirty(true);
    set_window_text_silent(g_text_edit, L"");
    refresh_time_edit();
    if (g_text_edit) SetFocus(g_text_edit);
}

void activate_existing_window() {
    if (!g_window || !IsWindow(g_window)) return;
    if (IsIconic(g_window)) ShowWindow(g_window, SW_RESTORE);
    ShowWindow(g_window, SW_SHOW);
    BringWindowToTop(g_window);
    SetForegroundWindow(g_window);
    HWND focus = GetFocus();
    if (!focus || !IsChild(g_window, focus)) SetFocus(g_time_edit ? g_time_edit : g_window);
}

LRESULT CALLBACK text_edit_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_GETDLGCODE) {
        MSG* keyMessage = reinterpret_cast<MSG*>(lp);
        if (keyMessage && keyMessage->message == WM_KEYDOWN && keyMessage->wParam == VK_RETURN) {
            return DefSubclassProc(wnd, msg, wp, lp) | DLGC_WANTMESSAGE;
        }
    }
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        append_current_line();
        return 0;
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

void reset_handles(HWND wnd) {
    if (wnd != g_window) return;
    if (g_text_edit && IsWindow(g_text_edit)) RemoveWindowSubclass(g_text_edit, text_edit_proc, 1);
    g_window = nullptr;
    g_time_edit = nullptr;
    g_refresh_time_button = nullptr;
    g_text_edit = nullptr;
    g_encoding_combo = nullptr;
    g_preview_edit = nullptr;
    g_save_button = nullptr;
    g_save_as_button = nullptr;
    g_open_button = nullptr;
    g_close_button = nullptr;
    g_current_path.clear();
    g_dirty = false;
    g_internal_text_change = false;
}

INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        g_window = wnd;
        g_time_edit = GetDlgItem(wnd, IDC_TIMESTAMP_TIME);
        g_refresh_time_button = GetDlgItem(wnd, IDC_TIMESTAMP_REFRESH_TIME);
        g_text_edit = GetDlgItem(wnd, IDC_TIMESTAMP_TEXT);
        g_encoding_combo = GetDlgItem(wnd, IDC_TIMESTAMP_ENCODING);
        g_save_button = GetDlgItem(wnd, IDC_TIMESTAMP_SAVE);
        g_save_as_button = GetDlgItem(wnd, IDC_TIMESTAMP_SAVE_AS);
        g_open_button = GetDlgItem(wnd, IDC_TIMESTAMP_OPEN);
        g_preview_edit = GetDlgItem(wnd, IDC_TIMESTAMP_PREVIEW);
        g_close_button = GetDlgItem(wnd, IDCANCEL);
        SetWindowTextW(wnd, L"\u6dfb\u52a0\u5f53\u524d\u65f6\u95f4\u6b4c\u8bcd");
        SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_TIMESTAMP_TIME), L"\u5f53\u524d\u65f6\u95f4");
        SetWindowTextW(g_refresh_time_button, L"\u5237\u65b0\u5f53\u524d\u65f6\u95f4(&T)");
        SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_TIMESTAMP_TEXT), L"\u5f53\u524d\u65f6\u95f4\u6b4c\u8bcd");
        SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_TIMESTAMP_ENCODING), L"\u7f16\u7801");
        SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_TIMESTAMP_PREVIEW), L"\u9884\u89c8");
        SetWindowTextW(g_save_button, L"\u4fdd\u5b58(&S)");
        SetWindowTextW(g_save_as_button, L"\u53e6\u5b58\u4e3a(&A)");
        SetWindowTextW(g_open_button, L"\u6253\u5f00(&O)");
        SetWindowTextW(g_close_button, L"\u5173\u95ed(&C)");
        set_accessible_name(g_time_edit, L"\u5f53\u524d\u65f6\u95f4");
        set_accessible_name(g_refresh_time_button, L"\u5237\u65b0\u5f53\u524d\u65f6\u95f4");
        set_accessible_name(g_text_edit, L"\u5f53\u524d\u65f6\u95f4\u6b4c\u8bcd");
        set_accessible_name(g_encoding_combo, L"\u7f16\u7801");
        set_accessible_name(g_save_button, L"\u4fdd\u5b58");
        set_accessible_name(g_save_as_button, L"\u53e6\u5b58\u4e3a");
        set_accessible_name(g_open_button, L"\u6253\u5f00");
        set_accessible_name(g_preview_edit, L"\u9884\u89c8");
        set_accessible_name(g_close_button, L"\u5173\u95ed");
        refresh_time_edit();
        init_encoding_combo("utf8");
        if (g_text_edit) SetWindowSubclass(g_text_edit, text_edit_proc, 1, 0);
        SetFocus(g_time_edit);
        return FALSE;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && LOWORD(wp) == IDC_TIMESTAMP_PREVIEW && !g_internal_text_change) {
            set_dirty(true);
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_TIMESTAMP_REFRESH_TIME:
            if (HIWORD(wp) == BN_CLICKED) {
                refresh_time_edit_and_speak();
                return TRUE;
            }
            break;
        case IDC_TIMESTAMP_SAVE:
            if (HIWORD(wp) == BN_CLICKED) {
                save_current(false);
                return TRUE;
            }
            break;
        case IDC_TIMESTAMP_SAVE_AS:
            if (HIWORD(wp) == BN_CLICKED) {
                save_current(true);
                return TRUE;
            }
            break;
        case IDC_TIMESTAMP_OPEN:
            if (HIWORD(wp) == BN_CLICKED) {
                open_lrc_file();
                return TRUE;
            }
            break;
        case IDCANCEL:
            if (confirm_save_if_dirty()) EndDialog(wnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        if (confirm_save_if_dirty()) EndDialog(wnd, IDCANCEL);
        return TRUE;
    case WM_NCDESTROY:
        reset_handles(wnd);
        return TRUE;
    }
    return FALSE;
}

}

void show_lyrics_timestamp_window(HWND parent) {
    if (g_window && IsWindow(g_window)) {
        activate_existing_window();
        return;
    }
    DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_ADD_TIMESTAMP_LRC), parent, dialog_proc, 0);
}
