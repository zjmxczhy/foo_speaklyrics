#include "stdafx.h"

#include "lyrics_jump_window.h"
#include "playback.h"
#include "speech_engine.h"

namespace {

const wchar_t* kWindowClassName = L"foo_speaklyrics_jump_window";
HWND g_window = nullptr;
HWND g_list = nullptr;
HWND g_close_button = nullptr;
std::vector<lyric_jump_item> g_items;

void set_window_accessible_name(HWND wnd, const wchar_t* name) {
    SetPropW(wnd, L"Name", reinterpret_cast<HANDLE>(const_cast<wchar_t*>(name)));
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, wnd, OBJID_CLIENT, CHILDID_SELF);
}

void refresh_list() {
    if (!g_list) return;

    int oldSelection = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    g_items = get_current_lyric_jump_items();

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (const auto& item : g_items) {
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.text.c_str()));
    }
    if (!g_items.empty()) {
        int selection = oldSelection >= 0 && oldSelection < static_cast<int>(g_items.size()) ? oldSelection : 0;
        SendMessageW(g_list, LB_SETCURSEL, static_cast<WPARAM>(selection), 0);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, nullptr, TRUE);
}

void activate_existing_window() {
    if (!g_window) return;
    if (IsIconic(g_window)) ShowWindow(g_window, SW_RESTORE);
    ShowWindow(g_window, SW_SHOW);
    SetForegroundWindow(g_window);
    SetFocus(g_list ? g_list : g_window);
}

void jump_selected() {
    if (!g_list) return;
    int selection = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= static_cast<int>(g_items.size())) return;
    jump_to_lyric_time_ms(g_items[selection].time_ms);
}

LRESULT CALLBACK window_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SYSCHAR:
        if (wp == L'c' || wp == L'C') {
            SendMessageW(wnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_CREATE: {
        g_window = wnd;
        g_list = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            WC_LISTBOXW,
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            0, 0, 0, 0,
            wnd,
            reinterpret_cast<HMENU>(1),
            core_api::get_my_instance(),
            nullptr);
        set_window_accessible_name(g_list, L"\u6B4C\u8BCD");
        g_close_button = CreateWindowExW(
            0,
            WC_BUTTONW,
            L"\u5173\u95ED(&C)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, 0, 0,
            wnd,
            reinterpret_cast<HMENU>(2),
            core_api::get_my_instance(),
            nullptr);
        refresh_list();
        SetFocus(g_list);
        return 0;
    }
    case WM_SIZE: {
        int width = LOWORD(lp);
        int height = HIWORD(lp);
        int buttonWidth = 88;
        int buttonHeight = 28;
        int margin = 8;
        if (g_close_button) {
            MoveWindow(g_close_button, width > buttonWidth + margin ? width - buttonWidth - margin : margin, height > buttonHeight + margin ? height - buttonHeight - margin : margin, buttonWidth, buttonHeight, TRUE);
        }
        if (g_list) {
            int listHeight = height > buttonHeight + margin * 3 ? height - buttonHeight - margin * 3 : 0;
            MoveWindow(g_list, margin, margin, width > margin * 2 ? width - margin * 2 : 0, listHeight, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == g_list && HIWORD(wp) == LBN_DBLCLK) {
            jump_selected();
            return 0;
        }
        if (LOWORD(wp) == 2 && HIWORD(wp) == BN_CLICKED) {
            SendMessageW(wnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        if (wnd == g_window) {
            g_list = nullptr;
            g_close_button = nullptr;
            g_window = nullptr;
            g_items.clear();
        }
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

LRESULT CALLBACK button_subclass_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_ESCAPE) {
            if (g_window) SendMessageW(g_window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            if (g_list) SetFocus(g_list);
            return 0;
        }
    }
    if (msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN) {
        if (wp == L'c' || wp == L'C') {
            if (g_window) SendMessageW(g_window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}

LRESULT CALLBACK list_subclass_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            jump_selected();
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (g_window) SendMessageW(g_window, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            if (g_close_button) SetFocus(g_close_button);
            return 0;
        }
    }
    if (msg == WM_SYSCHAR || msg == WM_SYSKEYDOWN) {
        if (wp == L'c' || wp == L'C') {
            if (g_window) SendMessageW(g_window, WM_CLOSE, 0, 0);
            return 0;
        }
    }
    return DefSubclassProc(wnd, msg, wp, lp);
}


void ensure_window_class() {
    static bool registered = false;
    if (registered) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = core_api::get_my_instance();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClassName;
    RegisterClassExW(&wc);
    registered = true;
}

}

void show_lyrics_jump_window(HWND parent) {
    auto items = get_current_lyric_jump_items();
    if (items.empty()) {
        popup_message::g_show("\xE5\xBD\x93\xE5\x89\x8D\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8F\xAF\xE7\x94\xA8\xE6\xAD\x8C\xE8\xAF\x8D", "\xE6\x8C\x89\xE6\xAD\x8C\xE8\xAF\x8D\xE8\xB7\xB3\xE8\xBD\xAC");
        speech_queue_speak(L"\u5F53\u524D\u6CA1\u6709\u53EF\u7528\u6B4C\u8BCD", true);
        if (g_window) refresh_lyrics_jump_window();
        return;
    }

    if (g_window && IsWindow(g_window)) {
        refresh_lyrics_jump_window();
        activate_existing_window();
        return;
    }

    ensure_window_class();
    g_window = CreateWindowExW(
        WS_EX_APPWINDOW,
        kWindowClassName,
        L"\u6309\u6B4C\u8BCD\u8DF3\u8F6C",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        520,
        420,
        parent,
        nullptr,
        core_api::get_my_instance(),
        nullptr);
    if (!g_window) return;
    if (g_list) SetWindowSubclass(g_list, list_subclass_proc, 1, 0);
    if (g_close_button) SetWindowSubclass(g_close_button, button_subclass_proc, 1, 0);
    ShowWindow(g_window, SW_SHOW);
    UpdateWindow(g_window);
    activate_existing_window();
}

void refresh_lyrics_jump_window() {
    if (!g_window || !IsWindow(g_window)) return;
    refresh_list();
}
