#include "stdafx.h"

#include "config.h"

#include "playback.h"

#include "resource.h"

#include "sapi_speech.h"





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

static std::vector<sapi_voice_info> g_sapi_voices;

static const int k_general_page_controls[] = {
    IDC_AUTO_SPEAK,
    IDC_STATIC_LRC_ENCODING,
    IDC_LRC_ENCODING,
    IDC_STATIC_LEAD,
    IDC_LEAD_MS,
    IDC_STATIC_MISSING_LRC_RETRY_SECONDS,
    IDC_MISSING_LRC_RETRY_SECONDS,
    IDC_STATIC_LYRIC_VALID_MS,
    IDC_LYRIC_VALID_MS,
};

static const int k_lyrics_page_controls[] = {
    IDC_STATIC_LYRIC_SOURCE_LIST,
    IDC_LYRIC_SOURCE_LIST,
    IDC_DOWNLOAD_TO_LRC_FOLDER,
    IDC_STATIC_FILE,
    IDC_LRC_FILE,
    IDC_BROWSE_FILE,
    IDC_STATIC_FOLDER,
    IDC_LRC_FOLDER,
    IDC_BROWSE_FOLDER,
    IDC_STATIC_TEMP_FOLDER,
    IDC_TEMP_LRC_FOLDER,
    IDC_BROWSE_TEMP_FOLDER,
    IDC_STATIC_TEMP_LRC_DELETE_DELAY_MS,
    IDC_TEMP_LRC_DELETE_DELAY_MS,
};

static const int k_tts_page_controls[] = {
    IDC_USE_SCREEN_READER,
    IDC_STATIC_TTS_VOICE,
    IDC_TTS_VOICE,
    IDC_STATIC_TTS_RATE,
    IDC_TTS_RATE,
    IDC_TTS_RATE_VALUE,
    IDC_STATIC_TTS_TEST_TEXT,
    IDC_TTS_TEST_TEXT,
    IDC_TTS_TEST,
};

static void set_apply_visible(HWND wnd, bool visible) {
    HWND apply = GetDlgItem(wnd, IDC_APPLY_SETTINGS);
    if (!apply) return;
    ShowWindow(apply, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(apply, visible ? TRUE : FALSE);
}

static void set_control_page_visible(HWND wnd, int id, bool visible) {
    HWND item = GetDlgItem(wnd, id);
    if (!item) return;
    ShowWindow(item, visible ? SW_SHOW : SW_HIDE);
    EnableWindow(item, visible ? TRUE : FALSE);
}

static void update_tts_detail_visibility(HWND wnd) {
    bool useScreenReader = IsDlgButtonChecked(wnd, IDC_USE_SCREEN_READER) == BST_CHECKED;
    const int detailControls[] = {
        IDC_STATIC_TTS_VOICE,
        IDC_TTS_VOICE,
        IDC_STATIC_TTS_RATE,
        IDC_TTS_RATE,
        IDC_TTS_RATE_VALUE,
        IDC_STATIC_TTS_TEST_TEXT,
        IDC_TTS_TEST_TEXT,
        IDC_TTS_TEST,
    };
    for (int id : detailControls) {
        set_control_page_visible(wnd, id, !useScreenReader);
    }
}

static void show_settings_page(HWND wnd, int page) {
    for (int id : k_general_page_controls) {
        set_control_page_visible(wnd, id, page == 0);
    }
    for (int id : k_lyrics_page_controls) {
        set_control_page_visible(wnd, id, page == 1);
    }
    for (int id : k_tts_page_controls) {
        set_control_page_visible(wnd, id, page == 2);
    }
    if (page == 2) update_tts_detail_visibility(wnd);
}

static void init_settings_tabs(HWND wnd) {
    HWND tab = GetDlgItem(wnd, IDC_SETTINGS_TAB);
    if (!tab) return;
    SetWindowTextW(tab, L"\u8bbe\u7f6e\u5206\u7c7b");

    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = const_cast<wchar_t*>(L"\u5e38\u89c4\u8bbe\u7f6e");
    TabCtrl_InsertItem(tab, 0, &item);
    item.pszText = const_cast<wchar_t*>(L"LRC\u6b4c\u8bcd");
    TabCtrl_InsertItem(tab, 1, &item);
    item.pszText = const_cast<wchar_t*>(L"TTS\u8bed\u97f3");
    TabCtrl_InsertItem(tab, 2, &item);
    TabCtrl_SetCurSel(tab, 0);
}

static int find_sapi_voice_index(const std::wstring& id) {
    if (id.empty()) return 0;
    for (int i = 0; i < static_cast<int>(g_sapi_voices.size()); ++i) {
        if (_wcsicmp(g_sapi_voices[i].id.c_str(), id.c_str()) == 0) return i;
    }
    return 0;
}

static int current_tts_rate(HWND wnd) {
    HWND slider = GetDlgItem(wnd, IDC_TTS_RATE);
    if (!slider) return 0;
    return static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
}

static void update_tts_rate_text(HWND wnd) {
    wchar_t text[32] = {};
    swprintf_s(text, L"%d", current_tts_rate(wnd));
    SetDlgItemTextW(wnd, IDC_TTS_RATE_VALUE, text);
}

static std::wstring current_tts_voice_name(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_TTS_VOICE);
    if (!combo || g_sapi_voices.empty()) return L"";
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_sapi_voices.size())) index = 0;
    return g_sapi_voices[index].name;
}

static std::wstring current_tts_voice_id(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_TTS_VOICE);
    if (!combo || g_sapi_voices.empty()) return L"";
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(g_sapi_voices.size())) index = 0;
    return g_sapi_voices[index].id;
}

static void update_tts_test_text(HWND wnd) {
    std::wstring voiceName = current_tts_voice_name(wnd);
    if (voiceName.empty()) voiceName = L"\u9ed8\u8ba4\u8bed\u97f3";
    wchar_t text[512] = {};
    swprintf_s(text, L"\u4f60\u9009\u62e9\u4e86%s\u8bed\u97f3\uff0c\u8bed\u901f%d\u3002", voiceName.c_str(), current_tts_rate(wnd));
    SetDlgItemTextW(wnd, IDC_TTS_TEST_TEXT, text);
}

static void init_tts_controls(HWND wnd) {
    CheckDlgButton(wnd, IDC_USE_SCREEN_READER, cfg_use_screen_reader.get() ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(wnd, IDC_USE_SCREEN_READER), L"\u4f7f\u7528\u5c4f\u5e55\u9605\u8bfb\u5668");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_VOICE), L"\u9009\u62e9\u8bed\u97f3");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_RATE), L"\u8bed\u901f");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_TEST_TEXT), L"\u8bd5\u542c\u6587\u672c");

    HWND combo = GetDlgItem(wnd, IDC_TTS_VOICE);
    if (combo) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        g_sapi_voices = sapi_enumerate_voices(L"sapi5");
        if (g_sapi_voices.empty()) {
            sapi_voice_info fallback;
            fallback.name = L"\u7cfb\u7edf\u9ed8\u8ba4\u8bed\u97f3";
            g_sapi_voices.push_back(fallback);
        }
        for (const auto& voice : g_sapi_voices) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(voice.name.c_str()));
        }
        std::wstring selected = cfg_to_wide(cfg_tts_voice_id);
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(find_sapi_voice_index(selected)), 0);
    }

    HWND slider = GetDlgItem(wnd, IDC_TTS_RATE);
    if (slider) {
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(-10, 10));
        SendMessageW(slider, TBM_SETTICFREQ, 1, 0);
        int rate = static_cast<int>(cfg_tts_rate.get());
        if (rate < -10) rate = -10;
        if (rate > 10) rate = 10;
        SendMessageW(slider, TBM_SETPOS, TRUE, rate);
    }
    update_tts_rate_text(wnd);
    update_tts_test_text(wnd);
    update_tts_detail_visibility(wnd);
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
    case IDC_TTS_TEST_TEXT:
        return true;
    default:
        return false;
    }
}
static void init_dialog(HWND wnd) {

    init_settings_tabs(wnd);

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

    CheckDlgButton(wnd, IDC_DOWNLOAD_TO_LRC_FOLDER, cfg_download_to_lrc_folder.get() ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(wnd, IDC_DOWNLOAD_TO_LRC_FOLDER), L"\u4e0b\u8f7d\u5230\u6307\u5b9a\u7684\u6b4c\u8bcd\u76ee\u5f55");

    int deleteDelayMs = static_cast<int>(cfg_temp_lrc_delete_delay_ms.get());
    if (deleteDelayMs < 0) deleteDelayMs = 0;
    SetDlgItemInt(wnd, IDC_TEMP_LRC_DELETE_DELAY_MS, static_cast<UINT>(deleteDelayMs), FALSE);

    init_source_list(wnd);

    init_tts_controls(wnd);

    show_settings_page(wnd, 0);

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

    cfg_download_to_lrc_folder.set(IsDlgButtonChecked(wnd, IDC_DOWNLOAD_TO_LRC_FOLDER) == BST_CHECKED);

    cfg_use_screen_reader.set(IsDlgButtonChecked(wnd, IDC_USE_SCREEN_READER) == BST_CHECKED);

    cfg_tts_voice_type.set("sapi5");

    set_cfg_from_wide(cfg_tts_voice_id, current_tts_voice_id(wnd));

    cfg_tts_rate.set(static_cast<int64_t>(current_tts_rate(wnd)));

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
        } else if (LOWORD(wp) == IDC_DOWNLOAD_TO_LRC_FOLDER && HIWORD(wp) == BN_CLICKED) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_USE_SCREEN_READER && HIWORD(wp) == BN_CLICKED) {
            update_tts_detail_visibility(wnd);
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_LRC_ENCODING && HIWORD(wp) == CBN_SELCHANGE) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_TTS_VOICE && HIWORD(wp) == CBN_SELCHANGE) {
            update_tts_test_text(wnd);
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

        case IDC_TTS_TEST:

            sapi_speak(L"sapi5", current_tts_voice_id(wnd).c_str(), current_tts_rate(wnd), get_dlg_text(wnd, IDC_TTS_TEST_TEXT).c_str(), true);

            return TRUE;

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
            } else if (header && header->idFrom == IDC_SETTINGS_TAB && header->code == TCN_SELCHANGE) {
                int page = static_cast<int>(TabCtrl_GetCurSel(GetDlgItem(wnd, IDC_SETTINGS_TAB)));
                show_settings_page(wnd, page);
                return TRUE;
            }
        }

        break;

    case WM_HSCROLL:

        if (reinterpret_cast<HWND>(lp) == GetDlgItem(wnd, IDC_TTS_RATE)) {
            update_tts_rate_text(wnd);
            update_tts_test_text(wnd);
            set_dialog_dirty(wnd, true);
            return TRUE;
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












