#include "stdafx.h"

#include "config.h"

#include "playback.h"

#include "resource.h"





static std::wstring cfg_to_wide(cfg_var_modern::cfg_string& s) {

    pfc::string8 v = s.get();

    return pfc::stringcvt::string_wide_from_utf8(v).get_ptr();

}



static void set_cfg_from_wide(cfg_var_modern::cfg_string& s, const std::wstring& v) {

    pfc::string8 utf8 = pfc::stringcvt::string_utf8_from_wide(v.c_str()).get_ptr();

    s.set(utf8);

}



static std::wstring get_dlg_text(HWND wnd, int id) {

    HWND item = GetDlgItem(wnd, id);

    int len = GetWindowTextLengthW(item);

    std::wstring out(static_cast<size_t>(len) + 1, L'\0');

    GetWindowTextW(item, out.data(), len + 1);

    out.resize(static_cast<size_t>(len));

    return out;

}



bool browse_lrc_file(HWND parent, pfc::string8& out) {

    wchar_t file[MAX_PATH * 4] = {};

    OPENFILENAMEW ofn = {};

    ofn.lStructSize = sizeof(ofn);

    ofn.hwndOwner = parent;

    ofn.lpstrFilter = L"LRC\u6b4c\u8bcd (*.lrc)\0*.lrc\0\u6240\u6709\u6587\u4ef6 (*.*)\0*.*\0";

    ofn.lpstrFile = file;

    ofn.nMaxFile = _countof(file);

    ofn.lpstrTitle = L"\u52a0\u8f7d\u672c\u5730LRC\u6b4c\u8bcd";

    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    ofn.lpstrDefExt = L"lrc";

    if (!GetOpenFileNameW(&ofn)) return false;

    out = pfc::stringcvt::string_utf8_from_wide(file).get_ptr();

    return true;

}



static bool browse_folder_with_title(HWND parent, const wchar_t* title, pfc::string8& out) {

    BROWSEINFOW bi = {};

    bi.hwndOwner = parent;

    bi.lpszTitle = title;

    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);

    if (!pidl) return false;

    wchar_t folder[MAX_PATH * 4] = {};

    bool ok = SHGetPathFromIDListW(pidl, folder) != FALSE;

    CoTaskMemFree(pidl);

    if (!ok) return false;

    out = pfc::stringcvt::string_utf8_from_wide(folder).get_ptr();

    return true;

}



bool browse_lrc_folder(HWND parent, pfc::string8& out) {

    return browse_folder_with_title(parent, L"\u9009\u62e9LRC\u6b4c\u8bcd\u76ee\u5f55", out);

}



bool browse_temp_lrc_folder(HWND parent, pfc::string8& out) {

    return browse_folder_with_title(parent, L"\u9009\u62e9LRC\u6b4c\u8bcd\u4e34\u65f6\u76ee\u5f55", out);

}





struct lyric_source_item {
    const char* id;
    const wchar_t* name;
};

static const lyric_source_item g_lyric_sources[] = {
    { "lrclib", L"LRCLIB" },
    { "qq1", L"QQ\u97f3\u4e501\u53f7\u6e90" },
    { "qq2", L"QQ\u97f3\u4e502\u53f7\u6e90" },
    { "netease", L"\u7f51\u6613\u4e91\u97f3\u4e50" },
};


struct lrc_encoding_item {
    const char* id;
    const wchar_t* name;
};

static const lrc_encoding_item g_lrc_encodings[] = {
    { "auto", L"\u81ea\u52a8" },
    { "utf8", L"Unicode (UTF-8)" },
    { "utf16le", L"Unicode (UTF-16 LE)" },
    { "utf16be", L"Unicode (UTF-16 BE)" },
    { "gbk", L"Simplified Chinese (GBK)" },
    { "gb18030", L"Chinese Simplified (GB18030)" },
    { "big5", L"Traditional Chinese (Big5)" },
    { "shiftjis", L"ANSI/OEM Japanese (Shift-JIS)" },
    { "korean", L"ANSI/OEM Korean (Unified Hangul Code)" },
    { "windows1252", L"Western European (Windows)" },
    { "windows1254", L"Turkish (Windows)" },
    { "acp", L"\u7cfb\u7edf\u9ed8\u8ba4 ANSI" },
};

static int find_lrc_encoding_index(const char* id) {
    if (!id || !*id) return 0;
    for (int i = 0; i < static_cast<int>(_countof(g_lrc_encodings)); ++i) {
        if (_stricmp(g_lrc_encodings[i].id, id) == 0) return i;
    }
    return 0;
}

static void init_lrc_encoding_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LRC_ENCODING);
    if (!combo) return;
    SetWindowTextW(combo, L"LRC\u6b4c\u8bcd\u7f16\u7801");
    for (const auto& item : g_lrc_encodings) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name));
    }
    pfc::string8 id = cfg_lrc_encoding.get();
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(find_lrc_encoding_index(id.get_ptr())), 0);
}

static void save_lrc_encoding_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LRC_ENCODING);
    if (!combo) return;
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(_countof(g_lrc_encodings))) index = 0;
    cfg_lrc_encoding.set(g_lrc_encodings[index].id);
}

static bool csv_has_token(const std::string& csv, const char* token) {
    size_t start = 0;
    while (start <= csv.size()) {
        size_t end = csv.find(',', start);
        if (end == std::string::npos) end = csv.size();
        std::string part = csv.substr(start, end - start);
        part.erase(std::remove_if(part.begin(), part.end(), [](unsigned char ch) { return ch <= ' '; }), part.end());
        std::transform(part.begin(), part.end(), part.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (part == token) return true;
        if (end == csv.size()) break;
        start = end + 1;
    }
    return false;
}

static void init_source_list(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_LYRIC_SOURCE_LIST);
    if (!list) return;
    SetWindowTextW(list, L"\u6b4c\u8bcd\u4e0b\u8f7d\u6765\u6e90");
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"\u6765\u6e90");
    col.cx = 300;
    ListView_InsertColumn(list, 0, &col);

    pfc::string8 cfg = cfg_lyric_sources.get();
    std::string selected = cfg.c_str();
    for (int i = 0; i < static_cast<int>(_countof(g_lyric_sources)); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<wchar_t*>(g_lyric_sources[i].name);
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, i, csv_has_token(selected, g_lyric_sources[i].id));
    }
}

static void save_source_list(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_LYRIC_SOURCE_LIST);
    if (!list) return;
    pfc::string8 csv;
    for (int i = 0; i < static_cast<int>(_countof(g_lyric_sources)); ++i) {
        if (ListView_GetCheckState(list, i)) {
            if (!csv.is_empty()) csv << ",";
            csv << g_lyric_sources[i].id;
        }
    }
    cfg_lyric_sources.set(csv);
}



static HWND g_settings_dialog_wnd = nullptr;

static void activate_settings_dialog() {
    if (!g_settings_dialog_wnd || !IsWindow(g_settings_dialog_wnd)) return;
    ShowWindow(g_settings_dialog_wnd, SW_SHOWNORMAL);
    BringWindowToTop(g_settings_dialog_wnd);
    SetForegroundWindow(g_settings_dialog_wnd);
    SetFocus(g_settings_dialog_wnd);
}
static bool g_settings_dialog_initializing = false;

static void set_apply_visible(HWND wnd, bool visible) {
    HWND apply = GetDlgItem(wnd, IDC_APPLY_SETTINGS);
    if (!apply) return;
    ShowWindow(apply, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(apply, visible ? TRUE : FALSE);
}

static void set_dialog_dirty(HWND wnd, bool dirty) {
    if (g_settings_dialog_initializing) return;
    SetWindowLongPtr(wnd, DWLP_USER, dirty ? 1 : 0);
    set_apply_visible(wnd, dirty);
}

static bool is_settings_edit_control(WORD id) {
    switch (id) {
    case IDC_LEAD_MS:
    case IDC_MISSING_LRC_RETRY_SECONDS:
    case IDC_LYRIC_VALID_MS:
    case IDC_LRC_FILE:
    case IDC_LRC_FOLDER:
    case IDC_TEMP_LRC_FOLDER:
    case IDC_TEMP_LRC_DELETE_DELAY_MS:
        return true;
    default:
        return false;
    }
}
static void init_dialog(HWND wnd) {

    CheckDlgButton(wnd, IDC_AUTO_SPEAK, cfg_auto_speak.get() ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemInt(wnd, IDC_LEAD_MS, static_cast<UINT>(cfg_lead_ms.get()), TRUE);

    int retryMs = static_cast<int>(cfg_missing_lrc_retry_ms.get());
    if (retryMs > 0 && retryMs < 100) retryMs *= 1000;
    if (retryMs <= 0) retryMs = 3000;
    SetDlgItemInt(wnd, IDC_MISSING_LRC_RETRY_SECONDS, static_cast<UINT>(retryMs), FALSE);

    SetDlgItemInt(wnd, IDC_LYRIC_VALID_MS, static_cast<UINT>(cfg_lyric_valid_ms.get()), FALSE);

    SetDlgItemTextW(wnd, IDC_LRC_FILE, L"");

    SetDlgItemTextW(wnd, IDC_LRC_FOLDER, cfg_to_wide(cfg_lrc_folder).c_str());

    SetDlgItemTextW(wnd, IDC_TEMP_LRC_FOLDER, cfg_to_wide(cfg_temp_lrc_folder).c_str());

    init_lrc_encoding_combo(wnd);

    int deleteDelayMs = static_cast<int>(cfg_temp_lrc_delete_delay_ms.get());
    if (deleteDelayMs < 0) deleteDelayMs = 0;
    SetDlgItemInt(wnd, IDC_TEMP_LRC_DELETE_DELAY_MS, static_cast<UINT>(deleteDelayMs), FALSE);

    init_source_list(wnd);


}



static void save_dialog(HWND wnd) {

    BOOL ok = FALSE;

    int lead = static_cast<int>(GetDlgItemInt(wnd, IDC_LEAD_MS, &ok, TRUE));

    BOOL retryOk = FALSE;

    int retryMs = static_cast<int>(GetDlgItemInt(wnd, IDC_MISSING_LRC_RETRY_SECONDS, &retryOk, FALSE));

    BOOL validOk = FALSE;

    int validMs = static_cast<int>(GetDlgItemInt(wnd, IDC_LYRIC_VALID_MS, &validOk, FALSE));

    BOOL deleteDelayOk = FALSE;

    int deleteDelayMs = static_cast<int>(GetDlgItemInt(wnd, IDC_TEMP_LRC_DELETE_DELAY_MS, &deleteDelayOk, FALSE));

    cfg_auto_speak.set(IsDlgButtonChecked(wnd, IDC_AUTO_SPEAK) == BST_CHECKED);

    cfg_lead_ms.set(ok ? static_cast<int64_t>(lead) : 0);

    if (retryOk && retryMs > 0) {
        if (retryMs < 500) retryMs = 500;
        if (retryMs > 600000) retryMs = 600000;
        cfg_missing_lrc_retry_ms.set(static_cast<int64_t>(retryMs));
    } else {
        cfg_missing_lrc_retry_ms.set(3000);
    }

    cfg_lyric_valid_ms.set((validOk && validMs > 0) ? static_cast<int64_t>(validMs) : 3000);

    if (deleteDelayOk && deleteDelayMs > 0) {
        if (deleteDelayMs > 600000) deleteDelayMs = 600000;
        cfg_temp_lrc_delete_delay_ms.set(static_cast<int64_t>(deleteDelayMs));
    } else {
        cfg_temp_lrc_delete_delay_ms.set(0);
    }

    std::wstring lrcFile = get_dlg_text(wnd, IDC_LRC_FILE);

    if (!lrcFile.empty()) {

        pfc::string8 utf8 = pfc::stringcvt::string_utf8_from_wide(lrcFile.c_str()).get_ptr();

        set_manual_lrc_file_for_current_track(utf8.get_ptr());

    }

    set_cfg_from_wide(cfg_lrc_folder, get_dlg_text(wnd, IDC_LRC_FOLDER));

    set_cfg_from_wide(cfg_temp_lrc_folder, get_dlg_text(wnd, IDC_TEMP_LRC_FOLDER));

    save_lrc_encoding_combo(wnd);

    save_source_list(wnd);

}



static INT_PTR CALLBACK dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {

    switch (msg) {

    case WM_INITDIALOG:

        g_settings_dialog_wnd = wnd;

        { INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES }; InitCommonControlsEx(&icc); }

        g_settings_dialog_initializing = true;

        init_dialog(wnd);

        g_settings_dialog_initializing = false;

        set_dialog_dirty(wnd, false);

        return TRUE;

    case WM_COMMAND:

        if (is_settings_edit_control(LOWORD(wp)) && HIWORD(wp) == EN_CHANGE) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_AUTO_SPEAK && HIWORD(wp) == BN_CLICKED) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_LRC_ENCODING && HIWORD(wp) == CBN_SELCHANGE) {
            set_dialog_dirty(wnd, true);
        }

        switch (LOWORD(wp)) {

        case IDC_BROWSE_FILE: {

            pfc::string8 path;

            if (browse_lrc_file(wnd, path)) {

                SetDlgItemTextW(wnd, IDC_LRC_FILE, pfc::stringcvt::string_wide_from_utf8(path).get_ptr());

                set_dialog_dirty(wnd, true);

            }

            return TRUE;

        }

        case IDC_BROWSE_FOLDER: {

            pfc::string8 path;

            if (browse_lrc_folder(wnd, path)) {

                SetDlgItemTextW(wnd, IDC_LRC_FOLDER, pfc::stringcvt::string_wide_from_utf8(path).get_ptr());

                set_dialog_dirty(wnd, true);

            }

            return TRUE;

        }

        case IDC_BROWSE_TEMP_FOLDER: {

            pfc::string8 path;

            if (browse_temp_lrc_folder(wnd, path)) {

                SetDlgItemTextW(wnd, IDC_TEMP_LRC_FOLDER, pfc::stringcvt::string_wide_from_utf8(path).get_ptr());

                set_dialog_dirty(wnd, true);

            }

            return TRUE;

        }

        case IDC_TEST_SPEAK:

            speak_test_message();

            return TRUE;

        case IDOK:

            save_dialog(wnd);

            EndDialog(wnd, IDOK);

            return TRUE;

        case IDC_APPLY_SETTINGS:

            save_dialog(wnd);

            SetFocus(GetDlgItem(wnd, IDOK));

            set_dialog_dirty(wnd, false);

            return TRUE;

        case IDCANCEL:

            EndDialog(wnd, IDCANCEL);

            return TRUE;

        }

        break;

    case WM_NCDESTROY:

        if (g_settings_dialog_wnd == wnd) g_settings_dialog_wnd = nullptr;

        break;

    case WM_NOTIFY:

        {
            NMHDR* header = reinterpret_cast<NMHDR*>(lp);
            if (header && header->idFrom == IDC_LYRIC_SOURCE_LIST && header->code == LVN_ITEMCHANGED) {
                set_dialog_dirty(wnd, true);
            }
        }

        break;

    }

    return FALSE;

}



void show_settings_dialog(HWND parent) {

    if (g_settings_dialog_wnd && IsWindow(g_settings_dialog_wnd)) {
        activate_settings_dialog();
        return;
    }

    DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_SETTINGS), parent, dialog_proc, 0);

}












