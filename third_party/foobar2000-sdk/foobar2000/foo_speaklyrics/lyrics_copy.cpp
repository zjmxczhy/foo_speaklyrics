#include "stdafx.h"

#include "config.h"
#include "lyrics_copy.h"
#include "playback.h"
#include "resource.h"
#include "speech_engine.h"

namespace {

lyric_copy_mode g_selected_dialog_mode = lyric_copy_mode::ask;
HWND g_copy_dialog_wnd = nullptr;

const lyric_copy_mode k_dialog_modes[] = {
    lyric_copy_mode::timestamps,
    lyric_copy_mode::plain,
    lyric_copy_mode::split,
};

void activate_copy_dialog() {
    if (!g_copy_dialog_wnd || !IsWindow(g_copy_dialog_wnd)) return;
    ShowWindow(g_copy_dialog_wnd, SW_SHOWNORMAL);
    BringWindowToTop(g_copy_dialog_wnd);
    SetForegroundWindow(g_copy_dialog_wnd);
    HWND list = GetDlgItem(g_copy_dialog_wnd, IDC_COPY_MODE_LIST);
    SetFocus(list ? list : g_copy_dialog_wnd);
}

std::wstring format_timestamp(int time_ms) {
    if (time_ms < 0) time_ms = 0;
    int minutes = time_ms / 60000;
    int seconds = (time_ms / 1000) % 60;
    int centiseconds = (time_ms % 1000) / 10;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"[%02d:%02d.%02d]", minutes, seconds, centiseconds);
    return buffer;
}

bool contains_split_char(const std::wstring& separators, wchar_t ch) {
    return !separators.empty() && separators.find(ch) != std::wstring::npos;
}

bool is_line_break(wchar_t ch) {
    return ch == L'\r' || ch == L'\n';
}

std::vector<lyric_jump_item> current_lyrics() {
    return get_current_lyric_jump_items();
}

std::wstring build_plain_text(const std::vector<lyric_jump_item>& items) {
    std::wstring text;
    for (const auto& item : items) {
        if (item.text.empty()) continue;
        text += item.text;
        text += L"\r\n";
    }
    return text;
}

std::wstring build_timestamp_text(const std::vector<lyric_jump_item>& items) {
    std::wstring text;
    for (const auto& item : items) {
        if (item.text.empty()) continue;
        text += format_timestamp(item.time_ms);
        text += item.text;
        text += L"\r\n";
    }
    return text;
}

std::wstring build_split_text(const std::vector<lyric_jump_item>& items) {
    std::wstring plain = build_plain_text(items);
    pfc::string8 configured = cfg_copy_split_separators.get();
    std::wstring separators = pfc::stringcvt::string_wide_from_utf8(configured.get_ptr()).get_ptr();
    if (separators.empty()) return plain;

    std::wstring text;
    text.reserve(plain.size() + 64);
    for (size_t i = 0; i < plain.size(); ++i) {
        wchar_t ch = plain[i];
        text.push_back(ch);
        if (contains_split_char(separators, ch)) {
            size_t next = i + 1;
            if (next < plain.size() && !is_line_break(plain[next])) text += L"\r\n";
        }
    }
    return text;
}

bool copy_text_with_message(const std::wstring& text) {
    if (text.empty()) {
        speech_queue_speak(L"\u5f53\u524dLRC\u6ca1\u6709\u53ef\u590d\u5236\u7684\u6b4c\u8bcd\u6587\u672c", true);
        return false;
    }

    bool ok = copy_text_to_clipboard(text);
    speech_queue_speak(ok ? L"\u6b4c\u8bcd\u590d\u5236\u6210\u529f" : L"\u6b4c\u8bcd\u590d\u5236\u5931\u8d25", true);
    return ok;
}

bool run_copy_mode(lyric_copy_mode mode) {
    std::vector<lyric_jump_item> items = current_lyrics();
    if (items.empty()) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u5df2\u52a0\u8f7d\u7684LRC\u6b4c\u8bcd", true);
        return false;
    }

    switch (mode) {
    case lyric_copy_mode::timestamps:
        return copy_text_with_message(build_timestamp_text(items));
    case lyric_copy_mode::plain:
        return copy_text_with_message(build_plain_text(items));
    case lyric_copy_mode::split:
        return copy_text_with_message(build_split_text(items));
    case lyric_copy_mode::ask:
    default:
        return false;
    }
}

void choose_current_dialog_item(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_COPY_MODE_LIST);
    int index = list ? static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)) : LB_ERR;
    if (index < 0 || index >= static_cast<int>(_countof(k_dialog_modes))) index = 0;
    g_selected_dialog_mode = k_dialog_modes[index];
    EndDialog(wnd, IDOK);
}

INT_PTR CALLBACK copy_dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_copy_dialog_wnd = wnd;
        SetWindowTextW(GetDlgItem(wnd, IDC_COPY_MODE_LIST), L"\u590d\u5236\u65b9\u5f0f");
        HWND list = GetDlgItem(wnd, IDC_COPY_MODE_LIST);
        if (list) {
            for (lyric_copy_mode mode : k_dialog_modes) {
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lyric_copy_mode_display_name(mode)));
            }
            SendMessageW(list, LB_SETCURSEL, 0, 0);
            SetFocus(list);
            return FALSE;
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COPY_MODE_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                choose_current_dialog_item(wnd);
                return TRUE;
            }
            break;
        case IDOK:
            choose_current_dialog_item(wnd);
            return TRUE;
        case IDCANCEL:
            EndDialog(wnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NCDESTROY:
        if (g_copy_dialog_wnd == wnd) g_copy_dialog_wnd = nullptr;
        break;
    }
    return FALSE;
}

}

const char* lyric_copy_mode_to_id(lyric_copy_mode mode) {
    switch (mode) {
    case lyric_copy_mode::timestamps: return "timestamps";
    case lyric_copy_mode::plain: return "plain";
    case lyric_copy_mode::split: return "split";
    case lyric_copy_mode::ask:
    default:
        return "ask";
    }
}

lyric_copy_mode lyric_copy_mode_from_id(const char* id) {
    if (!id || !*id) return lyric_copy_mode::ask;
    if (_stricmp(id, "timestamps") == 0) return lyric_copy_mode::timestamps;
    if (_stricmp(id, "plain") == 0) return lyric_copy_mode::plain;
    if (_stricmp(id, "split") == 0) return lyric_copy_mode::split;
    return lyric_copy_mode::ask;
}

const wchar_t* lyric_copy_mode_display_name(lyric_copy_mode mode) {
    switch (mode) {
    case lyric_copy_mode::timestamps: return L"\u590d\u5236\u65f6\u95f4\u6233\u6b4c\u8bcd";
    case lyric_copy_mode::plain: return L"\u590d\u5236\u65e0\u65f6\u95f4\u6233\u6b4c\u8bcd";
    case lyric_copy_mode::split: return L"\u590d\u5236\u5206\u884c\u6b4c\u8bcd";
    case lyric_copy_mode::ask:
    default:
        return L"\u6309\u7528\u6237\u9009\u62e9\u590d\u5236";
    }
}

bool copy_current_lyrics(lyric_copy_mode mode) {
    if (mode == lyric_copy_mode::ask) return show_copy_lyrics_menu(core_api::get_main_window());
    return run_copy_mode(mode);
}

bool show_copy_lyrics_menu(HWND parent) {
    if (g_copy_dialog_wnd && IsWindow(g_copy_dialog_wnd)) {
        activate_copy_dialog();
        return false;
    }

    g_selected_dialog_mode = lyric_copy_mode::ask;
    HWND owner = parent && IsWindow(parent) && IsWindowVisible(parent) ? parent : nullptr;
    INT_PTR result = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_COPY_LYRICS), owner, copy_dialog_proc, 0);
    if (result != IDOK || g_selected_dialog_mode == lyric_copy_mode::ask) return false;
    return run_copy_mode(g_selected_dialog_mode);
}
