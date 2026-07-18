#include "stdafx.h"

#include "config.h"

#include "playback.h"

#include "lrc_parser.h"

#include "lyrics_copy.h"

#include "resource.h"

#include "sapi_speech.h"

#include "speech_engine.h"

#include "speaklyrics_log.h"





static std::wstring cfg_to_wide(cfg_string& s) {

    pfc::string8 v = s.get();

    return pfc::stringcvt::string_wide_from_utf8(v).get_ptr();

}



static void set_cfg_from_wide(cfg_string& s, const std::wstring& v) {

    pfc::string8 utf8 = pfc::stringcvt::string_utf8_from_wide(v.c_str()).get_ptr();

    s.set(utf8.get_ptr());

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

struct announce_track_format_item {
    const char* id;
    const wchar_t* name;
};

static const announce_track_format_item g_announce_track_formats[] = {
    { "title_artist", L"\u6807\u9898\u548c\u827a\u672f\u5bb6" },
    { "title", L"\u6807\u9898" },
    { "artist_title", L"\u827a\u672f\u5bb6\u548c\u6807\u9898" },
    { "filename", L"\u6587\u4ef6\u540d" },
    { "filename_no_ext", L"\u6587\u4ef6\u540d\u4e0d\u542b\u6269\u5c55\u540d" },
};

static int find_announce_track_format_index(const char* id) {
    if (!id || !*id) return 0;
    for (int i = 0; i < static_cast<int>(_countof(g_announce_track_formats)); ++i) {
        if (_stricmp(g_announce_track_formats[i].id, id) == 0) return i;
    }
    return 0;
}

static void init_announce_track_format_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_ANNOUNCE_TRACK_FORMAT);
    if (!combo) return;
    SetWindowTextW(combo, L"\u5207\u6362\u6b4c\u66f2\u64ad\u62a5\u5185\u5bb9");
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : g_announce_track_formats) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name));
    }
    pfc::string8 id = cfg_announce_track_format.get();
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(find_announce_track_format_index(id.get_ptr())), 0);
}

static void save_announce_track_format_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_ANNOUNCE_TRACK_FORMAT);
    if (!combo) return;
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(_countof(g_announce_track_formats))) index = 0;
    cfg_announce_track_format.set(g_announce_track_formats[index].id);
}


static void init_lrc_encoding_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LRC_ENCODING);
    if (!combo) return;
    SetWindowTextW(combo, L"LRC\u6b4c\u8bcd\u7f16\u7801");
    size_t count = 0;
    const lrc_encoding_info* encodings = lrc_get_encoding_options(count);
    for (size_t i = 0; i < count; ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(encodings[i].name));
    }
    pfc::string8 id = cfg_lrc_encoding.get();
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(lrc_find_encoding_index(id.get_ptr())), 0);
}

static void save_lrc_encoding_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LRC_ENCODING);
    if (!combo) return;
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    size_t count = 0;
    const lrc_encoding_info* encodings = lrc_get_encoding_options(count);
    if (index < 0 || index >= static_cast<int>(count)) index = 0;
    cfg_lrc_encoding.set(encodings[index].id);
}

struct lyric_speak_mode_item {
    const char* id;
    const wchar_t* name;
};

static const lyric_speak_mode_item g_lyric_speak_modes[] = {
    { "timestamp", L"\u6717\u8bfb\u65f6\u95f4\u6233\u6b4c\u8bcd" },
    { "advance", L"\u52a8\u6001\u63d0\u524d\u6717\u8bfb\u6b4c\u8bcd" },
};

static void init_lyric_speak_mode_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LYRIC_SPEAK_MODE);
    if (!combo) return;
    SetWindowTextW(combo, L"\u6b4c\u8bcd\u6717\u8bfb\u65b9\u5f0f");
    pfc::string8 configured = cfg_lyric_speak_mode.get();
    int selected = 0;
    for (size_t i = 0; i < _countof(g_lyric_speak_modes); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(g_lyric_speak_modes[i].name));
        if (_stricmp(configured.get_ptr(), g_lyric_speak_modes[i].id) == 0) selected = static_cast<int>(i);
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

static void save_lyric_speak_mode_combo(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_LYRIC_SPEAK_MODE);
    if (!combo) return;
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(_countof(g_lyric_speak_modes))) index = 0;
    cfg_lyric_speak_mode.set(g_lyric_speak_modes[index].id);
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
    cfg_lyric_sources.set(csv.get_ptr());
}

static const lyric_copy_mode g_copy_modes[] = {
    lyric_copy_mode::ask,
    lyric_copy_mode::timestamps,
    lyric_copy_mode::plain,
    lyric_copy_mode::split,
};

static int find_copy_mode_index(const char* id) {
    lyric_copy_mode mode = lyric_copy_mode_from_id(id);
    for (int i = 0; i < static_cast<int>(_countof(g_copy_modes)); ++i) {
        if (g_copy_modes[i] == mode) return i;
    }
    return 0;
}

static lyric_copy_mode current_copy_mode(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_COPY_MODE);
    if (!combo) return lyric_copy_mode::ask;
    int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(_countof(g_copy_modes))) index = 0;
    return g_copy_modes[index];
}

static void init_copy_mode_controls(HWND wnd) {
    HWND combo = GetDlgItem(wnd, IDC_COPY_MODE);
    if (!combo) return;
    SetWindowTextW(combo, L"\u590d\u5236\u6b4c\u8bcd\u9ed8\u8ba4\u65b9\u5f0f");
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (lyric_copy_mode mode : g_copy_modes) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lyric_copy_mode_display_name(mode)));
    }
    pfc::string8 id = cfg_copy_mode.get();
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(find_copy_mode_index(id.get_ptr())), 0);
    SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_COPY_MODE), L"\u590d\u5236\u6b4c\u8bcd\u9ed8\u8ba4\u65b9\u5f0f");
    SetWindowTextW(GetDlgItem(wnd, IDC_SET_COPY_DEFAULT), L"\u8bbe\u4e3a\u9ed8\u8ba4(&F)");
    SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_COPY_SPLIT_SEPARATORS), L"\u6b4c\u8bcd\u5206\u884c\u7b26\u53f7");
    SetWindowTextW(GetDlgItem(wnd, IDC_COPY_SPLIT_SEPARATORS), L"\u6b4c\u8bcd\u5206\u884c\u7b26\u53f7");
    SetDlgItemTextW(wnd, IDC_COPY_SPLIT_SEPARATORS, cfg_to_wide(cfg_copy_split_separators).c_str());
}

static void save_copy_mode_controls(HWND wnd) {
    cfg_copy_mode.set(lyric_copy_mode_to_id(current_copy_mode(wnd)));
    set_cfg_from_wide(cfg_copy_split_separators, get_dlg_text(wnd, IDC_COPY_SPLIT_SEPARATORS));
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

struct screen_reader_option {
    const char* id;
    const wchar_t* name;
};

static const screen_reader_option g_screen_reader_targets[] = {
    { "auto", L"\u81ea\u52a8" },
    { "boy", L"\u4fdd\u76ca" },
    { "zdsr", L"\u4e89\u6e21" },
};

static const char* g_screen_reader_channel_mode_ids[] = {
    "main",
    "shared",
    "named",
};

struct screen_reader_channel_profile {
    int mode = 2;
    std::wstring name = L"\u6717\u8bfb\u6b4c\u8bcd\u901a\u9053";
};

static screen_reader_channel_profile g_boy_channel_profile;
static screen_reader_channel_profile g_zdsr_channel_profile;
static int g_current_screen_reader_target = 0;

static int find_screen_reader_option(const screen_reader_option* options, size_t count, const char* id, int fallback) {
    if (id && *id) {
        for (size_t i = 0; i < count; ++i) {
            if (_stricmp(options[i].id, id) == 0) return static_cast<int>(i);
        }
    }
    return fallback;
}

static int current_screen_reader_target(HWND wnd) {
    int index = static_cast<int>(SendDlgItemMessageW(wnd, IDC_SCREEN_READER_TARGET, CB_GETCURSEL, 0, 0));
    return index >= 0 && index < static_cast<int>(_countof(g_screen_reader_targets)) ? index : 0;
}

static int find_screen_reader_channel_mode(const char* id) {
    if (id && *id) {
        for (size_t i = 0; i < _countof(g_screen_reader_channel_mode_ids); ++i) {
            if (_stricmp(g_screen_reader_channel_mode_ids[i], id) == 0) return static_cast<int>(i);
        }
    }
    return 2;
}

static screen_reader_channel_profile* screen_reader_profile_for_target(int target) {
    if (target == 1) return &g_boy_channel_profile;
    if (target == 2) return &g_zdsr_channel_profile;
    return nullptr;
}

static std::wstring clean_channel_input(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return ch == L'\r' || ch == L'\n' || ch < L' ';
    }), value.end());
    while (!value.empty() && value.front() <= L' ') value.erase(value.begin());
    while (!value.empty() && value.back() <= L' ') value.pop_back();
    if (value.size() > 64) value.resize(64);
    return value;
}

static void capture_screen_reader_channel_profile(HWND wnd, int target) {
    screen_reader_channel_profile* profile = screen_reader_profile_for_target(target);
    if (!profile) return;
    std::wstring value = clean_channel_input(get_dlg_text(wnd, IDC_SCREEN_READER_CHANNEL_NAME));
    if (_wcsicmp(value.c_str(), L"\u4e3b\u901a\u9053") == 0) {
        profile->mode = 0;
    } else if (_wcsicmp(value.c_str(), L"\u4e09\u65b9\u5171\u4eab\u901a\u9053") == 0) {
        profile->mode = 1;
    } else {
        profile->mode = 2;
        profile->name = value.empty() ? L"\u6717\u8bfb\u6b4c\u8bcd\u901a\u9053" : value;
    }
}

static void update_screen_reader_channel_enabled_state(HWND wnd) {
    bool useScreenReader = IsDlgButtonChecked(wnd, IDC_USE_SCREEN_READER) == BST_CHECKED;
    int target = current_screen_reader_target(wnd);
    bool hasChannelSettings = useScreenReader && target != 0;
    EnableWindow(GetDlgItem(wnd, IDC_SCREEN_READER_TARGET), useScreenReader ? TRUE : FALSE);
    EnableWindow(GetDlgItem(wnd, IDC_STATIC_SCREEN_READER_CHANNEL_NAME), hasChannelSettings ? TRUE : FALSE);
    EnableWindow(GetDlgItem(wnd, IDC_SCREEN_READER_CHANNEL_NAME), hasChannelSettings ? TRUE : FALSE);
}

static void load_screen_reader_channel_profile(HWND wnd, int target) {
    screen_reader_channel_profile* profile = screen_reader_profile_for_target(target);
    if (profile) {
        const wchar_t* value = profile->mode == 0 ? L"\u4e3b\u901a\u9053" :
            profile->mode == 1 ? L"\u4e09\u65b9\u5171\u4eab\u901a\u9053" : profile->name.c_str();
        SetDlgItemTextW(wnd, IDC_SCREEN_READER_CHANNEL_NAME, value);
    } else {
        SetDlgItemTextW(wnd, IDC_SCREEN_READER_CHANNEL_NAME, L"");
    }
    update_screen_reader_channel_enabled_state(wnd);
}

static void init_screen_reader_channel_controls(HWND wnd) {
    HWND targetCombo = GetDlgItem(wnd, IDC_SCREEN_READER_TARGET);
    if (!targetCombo) return;

    SetWindowTextW(targetCombo, L"\u6717\u8bfb\u63a5\u53e3");
    SetWindowTextW(GetDlgItem(wnd, IDC_SCREEN_READER_CHANNEL_NAME), L"\u901a\u9053\u540d\u79f0");
    SendMessageW(GetDlgItem(wnd, IDC_SCREEN_READER_CHANNEL_NAME), EM_SETLIMITTEXT, 64, 0);

    SendMessageW(targetCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& item : g_screen_reader_targets) {
        SendMessageW(targetCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.name));
    }
    pfc::string8 boyMode = cfg_boy_channel_mode.get();
    pfc::string8 zdsrMode = cfg_zdsr_channel_mode.get();
    g_boy_channel_profile.mode = find_screen_reader_channel_mode(boyMode.get_ptr());
    g_boy_channel_profile.name = cfg_to_wide(cfg_boy_channel_name);
    g_zdsr_channel_profile.mode = find_screen_reader_channel_mode(zdsrMode.get_ptr());
    g_zdsr_channel_profile.name = cfg_to_wide(cfg_zdsr_channel_name);

    pfc::string8 target = cfg_screen_reader_channel_target.get();
    g_current_screen_reader_target = find_screen_reader_option(g_screen_reader_targets, _countof(g_screen_reader_targets), target.get_ptr(), 0);
    SendMessageW(targetCombo, CB_SETCURSEL, static_cast<WPARAM>(g_current_screen_reader_target), 0);
    load_screen_reader_channel_profile(wnd, g_current_screen_reader_target);
}

static std::wstring normalized_channel_name(std::wstring value) {
    value = clean_channel_input(std::move(value));
    if (value.empty()) value = L"\u6717\u8bfb\u6b4c\u8bcd\u901a\u9053";
    return value;
}

static bool save_screen_reader_channel_controls(HWND wnd) {
    capture_screen_reader_channel_profile(wnd, g_current_screen_reader_target);
    g_boy_channel_profile.name = normalized_channel_name(g_boy_channel_profile.name);
    g_zdsr_channel_profile.name = normalized_channel_name(g_zdsr_channel_profile.name);

    pfc::string8 oldBoyMode = cfg_boy_channel_mode.get();
    pfc::string8 oldZdsrMode = cfg_zdsr_channel_mode.get();
    bool profilesChanged =
        _stricmp(oldBoyMode.get_ptr(), g_screen_reader_channel_mode_ids[g_boy_channel_profile.mode]) != 0 ||
        _stricmp(oldZdsrMode.get_ptr(), g_screen_reader_channel_mode_ids[g_zdsr_channel_profile.mode]) != 0 ||
        cfg_to_wide(cfg_boy_channel_name) != g_boy_channel_profile.name ||
        cfg_to_wide(cfg_zdsr_channel_name) != g_zdsr_channel_profile.name;

    int target = current_screen_reader_target(wnd);
    cfg_screen_reader_channel_target.set(g_screen_reader_targets[target].id);
    cfg_boy_channel_mode.set(g_screen_reader_channel_mode_ids[g_boy_channel_profile.mode]);
    cfg_zdsr_channel_mode.set(g_screen_reader_channel_mode_ids[g_zdsr_channel_profile.mode]);
    set_cfg_from_wide(cfg_boy_channel_name, g_boy_channel_profile.name);
    set_cfg_from_wide(cfg_zdsr_channel_name, g_zdsr_channel_profile.name);
    return profilesChanged;
}

static const int k_general_page_controls[] = {
    IDC_AUTO_SPEAK,
    IDC_ANNOUNCE_TRACK,
    IDC_STATIC_ANNOUNCE_TRACK_FORMAT,
    IDC_ANNOUNCE_TRACK_FORMAT,
    IDC_STATIC_ANNOUNCE_TRACK_DELAY_MS,
    IDC_ANNOUNCE_TRACK_DELAY_MS,
    IDC_STATIC_LRC_ENCODING,
    IDC_LRC_ENCODING,
    IDC_STATIC_LYRIC_SPEAK_MODE,
    IDC_LYRIC_SPEAK_MODE,
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
    IDC_STATIC_COPY_MODE,
    IDC_COPY_MODE,
    IDC_SET_COPY_DEFAULT,
    IDC_STATIC_COPY_SPLIT_SEPARATORS,
    IDC_COPY_SPLIT_SEPARATORS,
    IDC_COPY_FILTER_LEADING_CREDITS,
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
    IDC_STATIC_SCREEN_READER_TARGET,
    IDC_SCREEN_READER_TARGET,
    IDC_STATIC_SCREEN_READER_CHANNEL_NAME,
    IDC_SCREEN_READER_CHANNEL_NAME,
    IDC_STATIC_SCREEN_READER_CHANNEL_NOTE,
    IDC_STATIC_TTS_VOICE,
    IDC_TTS_VOICE,
    IDC_STATIC_TTS_RATE,
    IDC_TTS_RATE,
    IDC_TTS_RATE_VALUE,
    IDC_STATIC_TTS_TEST_TEXT,
    IDC_TTS_TEST_TEXT,
    IDC_TTS_TEST,
};

static const int k_about_page_controls[] = {
    IDC_STATIC_ABOUT_PURPOSE,
    IDC_ABOUT_PURPOSE,
    IDC_ABOUT_GITHUB_REPO,
    IDC_ABOUT_GITHUB_RELEASES,
    IDC_ABOUT_GITEE_RELEASES,
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
    const int sapiControls[] = {
        IDC_STATIC_TTS_VOICE,
        IDC_TTS_VOICE,
        IDC_STATIC_TTS_RATE,
        IDC_TTS_RATE,
        IDC_TTS_RATE_VALUE,
        IDC_STATIC_TTS_TEST_TEXT,
        IDC_TTS_TEST_TEXT,
        IDC_TTS_TEST,
    };
    const int screenReaderControls[] = {
        IDC_STATIC_SCREEN_READER_TARGET,
        IDC_SCREEN_READER_TARGET,
        IDC_STATIC_SCREEN_READER_CHANNEL_NAME,
        IDC_SCREEN_READER_CHANNEL_NAME,
        IDC_STATIC_SCREEN_READER_CHANNEL_NOTE,
    };
    for (int id : sapiControls) {
        set_control_page_visible(wnd, id, !useScreenReader);
    }
    for (int id : screenReaderControls) {
        set_control_page_visible(wnd, id, useScreenReader);
    }
    update_screen_reader_channel_enabled_state(wnd);
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
    for (int id : k_about_page_controls) {
        set_control_page_visible(wnd, id, page == 3);
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
    item.pszText = const_cast<wchar_t*>(L"\u5173\u4e8e");
    TabCtrl_InsertItem(tab, 3, &item);
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
    SetWindowTextW(GetDlgItem(wnd, IDC_USE_SCREEN_READER), L"\u4f7f\u7528\u5c4f\u5e55\u9605\u8bfb\u5668(&R)");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_VOICE), L"\u9009\u62e9\u8bed\u97f3");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_RATE), L"\u8bed\u901f");
    SetWindowTextW(GetDlgItem(wnd, IDC_TTS_TEST_TEXT), L"\u8bd5\u542c\u6587\u672c");

    init_screen_reader_channel_controls(wnd);

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

static void init_about_controls(HWND wnd) {
    SetWindowTextW(GetDlgItem(wnd, IDC_ABOUT_PURPOSE), L"\u4e3b\u8981\u7528\u9014");
    SetDlgItemTextW(wnd, IDC_ABOUT_PURPOSE,
        L"\u5728\u64ad\u653e\u6b4c\u66f2\u65f6\u8bfb\u53d6 LRC \u6b4c\u8bcd\uff0c"
        L"\u5e76\u901a\u8fc7\u5c4f\u5e55\u9605\u8bfb\u5668/Tolk \u6216 SAPI5.1 \u6717\u8bfb\u5f53\u524d\u6b4c\u8bcd\u3002");
}

static void open_url(HWND wnd, const wchar_t* url) {
    ShellExecuteW(wnd, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
}

static void set_dialog_dirty(HWND wnd, bool dirty) {
    if (g_settings_dialog_initializing) return;
    SetWindowLongPtr(wnd, DWLP_USER, dirty ? 1 : 0);
    set_apply_visible(wnd, dirty);
}

static bool is_settings_edit_control(WORD id) {
    switch (id) {
    case IDC_LEAD_MS:
    case IDC_ANNOUNCE_TRACK_DELAY_MS:
    case IDC_MISSING_LRC_RETRY_SECONDS:
    case IDC_LYRIC_VALID_MS:
    case IDC_LRC_FILE:
    case IDC_LRC_FOLDER:
    case IDC_TEMP_LRC_FOLDER:
    case IDC_TEMP_LRC_DELETE_DELAY_MS:
    case IDC_COPY_SPLIT_SEPARATORS:
    case IDC_SCREEN_READER_CHANNEL_NAME:
    case IDC_TTS_TEST_TEXT:
        return true;
    default:
        return false;
    }
}
static void init_dialog(HWND wnd) {

    init_settings_tabs(wnd);

    CheckDlgButton(wnd, IDC_AUTO_SPEAK, cfg_auto_speak.get() ? BST_CHECKED : BST_UNCHECKED);

    CheckDlgButton(wnd, IDC_ANNOUNCE_TRACK, cfg_announce_track_on_change.get() ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(wnd, IDC_ANNOUNCE_TRACK), L"\u5207\u6362\u6b4c\u66f2\u65f6\u64ad\u62a5\u6b4c\u66f2\u4fe1\u606f");
    SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_ANNOUNCE_TRACK_FORMAT), L"\u5207\u6362\u6b4c\u66f2\u64ad\u62a5\u5185\u5bb9");
    SetWindowTextW(GetDlgItem(wnd, IDC_STATIC_ANNOUNCE_TRACK_DELAY_MS), L"\u5207\u6362\u6b4c\u66f2\u64ad\u62a5\u5ef6\u8fdf\u6beb\u79d2");
    init_announce_track_format_combo(wnd);
    int announceDelayMs = static_cast<int>(cfg_announce_track_delay_ms.get());
    if (announceDelayMs < 0) announceDelayMs = 0;
    if (announceDelayMs > 10000) announceDelayMs = 10000;
    SetDlgItemInt(wnd, IDC_ANNOUNCE_TRACK_DELAY_MS, static_cast<UINT>(announceDelayMs), FALSE);

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

    init_lyric_speak_mode_combo(wnd);

    init_copy_mode_controls(wnd);

    CheckDlgButton(wnd, IDC_COPY_FILTER_LEADING_CREDITS, cfg_copy_filter_leading_credits.get() ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(wnd, IDC_COPY_FILTER_LEADING_CREDITS), L"\u590d\u5236\u6b4c\u8bcd\u65f6\u8fc7\u6ee4\u7247\u5934\u7f72\u540d(&H)");

    int deleteDelayMs = static_cast<int>(cfg_temp_lrc_delete_delay_ms.get());
    if (deleteDelayMs < 0) deleteDelayMs = 0;
    SetDlgItemInt(wnd, IDC_TEMP_LRC_DELETE_DELAY_MS, static_cast<UINT>(deleteDelayMs), FALSE);

    init_source_list(wnd);

    init_tts_controls(wnd);

    init_about_controls(wnd);

    show_settings_page(wnd, 0);

}



static INT_PTR CALLBACK restart_confirm_dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(wnd, LOWORD(wp));
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(wnd, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static bool confirm_foobar_restart(HWND parent) {
    INT_PTR result = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_RESTART_CONFIRM),
        parent, restart_confirm_dialog_proc, 0);
    return result == IDOK;
}

static bool execute_main_command_direct(const GUID& command) {
    for (auto item : mainmenu_commands::enumerate()) {
        const t_uint32 count = item->get_command_count();
        for (t_uint32 index = 0; index < count; ++index) {
            if (item->get_command(index) != command) continue;
            try {
                item->execute(index, service_ptr_t<service_base>());
                return true;
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}

static void queue_foobar_restart() {
    fb2k::inMainThread([] {
        speaklyrics_log_info(L"读屏通道设置：用户确认重启 foobar2000。");
        if (execute_main_command_direct(standard_commands::guid_main_restart)) return;

        speaklyrics_log_error(L"读屏通道设置：无法调用 foobar2000 官方重启命令。");
        pfc::string8 message = pfc::stringcvt::string_utf8_from_wide(
            L"无法自动重启 foobar2000，请手动退出并重新启动。");
        pfc::string8 title = pfc::stringcvt::string_utf8_from_wide(L"朗读歌词");
        popup_message::g_show(message.get_ptr(), title.get_ptr());
    });
}

static bool save_dialog(HWND wnd) {

    BOOL ok = FALSE;

    int lead = static_cast<int>(GetDlgItemInt(wnd, IDC_LEAD_MS, &ok, TRUE));

    BOOL announceDelayOk = FALSE;

    int announceDelayMs = static_cast<int>(GetDlgItemInt(wnd, IDC_ANNOUNCE_TRACK_DELAY_MS, &announceDelayOk, FALSE));

    BOOL retryOk = FALSE;

    int retryMs = static_cast<int>(GetDlgItemInt(wnd, IDC_MISSING_LRC_RETRY_SECONDS, &retryOk, FALSE));

    BOOL validOk = FALSE;

    int validMs = static_cast<int>(GetDlgItemInt(wnd, IDC_LYRIC_VALID_MS, &validOk, FALSE));

    BOOL deleteDelayOk = FALSE;

    int deleteDelayMs = static_cast<int>(GetDlgItemInt(wnd, IDC_TEMP_LRC_DELETE_DELAY_MS, &deleteDelayOk, FALSE));

    cfg_auto_speak = (IsDlgButtonChecked(wnd, IDC_AUTO_SPEAK) == BST_CHECKED);

    cfg_announce_track_on_change = (IsDlgButtonChecked(wnd, IDC_ANNOUNCE_TRACK) == BST_CHECKED);

    save_announce_track_format_combo(wnd);

    if (announceDelayOk && announceDelayMs > 0) {
        if (announceDelayMs > 10000) announceDelayMs = 10000;
        cfg_announce_track_delay_ms = announceDelayMs;
    } else {
        cfg_announce_track_delay_ms = 0;
    }

    cfg_lead_ms = ok ? static_cast<int>(lead) : 0;

    if (retryOk && retryMs > 0) {
        if (retryMs < 500) retryMs = 500;
        if (retryMs > 600000) retryMs = 600000;
        cfg_missing_lrc_retry_ms = retryMs;
    } else {
        cfg_missing_lrc_retry_ms = 3000;
    }

    cfg_lyric_valid_ms = (validOk && validMs > 0) ? validMs : 3000;

    if (deleteDelayOk && deleteDelayMs > 0) {
        if (deleteDelayMs > 600000) deleteDelayMs = 600000;
        cfg_temp_lrc_delete_delay_ms = deleteDelayMs;
    } else {
        cfg_temp_lrc_delete_delay_ms = 0;
    }

    std::wstring lrcFile = get_dlg_text(wnd, IDC_LRC_FILE);

    if (!lrcFile.empty()) {

        pfc::string8 utf8 = pfc::stringcvt::string_utf8_from_wide(lrcFile.c_str()).get_ptr();

        set_manual_lrc_file_for_current_track(utf8.get_ptr());

    }

    set_cfg_from_wide(cfg_lrc_folder, get_dlg_text(wnd, IDC_LRC_FOLDER));

    set_cfg_from_wide(cfg_temp_lrc_folder, get_dlg_text(wnd, IDC_TEMP_LRC_FOLDER));

    save_lrc_encoding_combo(wnd);

    save_lyric_speak_mode_combo(wnd);

    save_source_list(wnd);

    save_copy_mode_controls(wnd);

    cfg_copy_filter_leading_credits = (IsDlgButtonChecked(wnd, IDC_COPY_FILTER_LEADING_CREDITS) == BST_CHECKED);

    cfg_use_screen_reader = (IsDlgButtonChecked(wnd, IDC_USE_SCREEN_READER) == BST_CHECKED);

    bool channelProfilesChanged = save_screen_reader_channel_controls(wnd);

    cfg_tts_voice_type.set("sapi5");

    set_cfg_from_wide(cfg_tts_voice_id, current_tts_voice_id(wnd));

    cfg_tts_rate = current_tts_rate(wnd);

    bool restartRequested = false;
    if (!write_screen_reader_channel_config_files()) {
        pfc::string8 message = pfc::stringcvt::string_utf8_from_wide(
            L"\u8bfb\u5c4f\u901a\u9053\u914d\u7f6e\u65e0\u6cd5\u5199\u5165\u3002"
            L"\u8bf7\u68c0\u67e5\u7ec4\u4ef6\u76ee\u5f55\u6743\u9650\uff1b\u4e89\u6e21\u901a\u9053\u540d\u79f0\u8bf7\u4f7f\u7528 GBK \u53ef\u8868\u793a\u7684\u5b57\u7b26\u3002");
        popup_message::g_show(message.get_ptr(), "\xE6\x9C\x97\xE8\xAF\xBB\xE6\xAD\x8C\xE8\xAF\x8D");
    } else if (channelProfilesChanged) {
        restartRequested = confirm_foobar_restart(wnd);
    }

    if (!restartRequested) speech_preload();

    return restartRequested;
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
            if (LOWORD(wp) == IDC_SCREEN_READER_CHANNEL_NAME) {
                capture_screen_reader_channel_profile(wnd, g_current_screen_reader_target);
            }
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_AUTO_SPEAK && HIWORD(wp) == BN_CLICKED) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_ANNOUNCE_TRACK && HIWORD(wp) == BN_CLICKED) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_COPY_MODE && HIWORD(wp) == CBN_SELCHANGE) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_COPY_FILTER_LEADING_CREDITS && HIWORD(wp) == BN_CLICKED) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_USE_SCREEN_READER && HIWORD(wp) == BN_CLICKED) {
            update_tts_detail_visibility(wnd);
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_SCREEN_READER_TARGET && HIWORD(wp) == CBN_SELCHANGE) {
            capture_screen_reader_channel_profile(wnd, g_current_screen_reader_target);
            g_current_screen_reader_target = current_screen_reader_target(wnd);
            load_screen_reader_channel_profile(wnd, g_current_screen_reader_target);
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_LRC_ENCODING && HIWORD(wp) == CBN_SELCHANGE) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_LYRIC_SPEAK_MODE && HIWORD(wp) == CBN_SELCHANGE) {
            set_dialog_dirty(wnd, true);
        } else if (LOWORD(wp) == IDC_ANNOUNCE_TRACK_FORMAT && HIWORD(wp) == CBN_SELCHANGE) {
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

        case IDC_SET_COPY_DEFAULT:

            save_copy_mode_controls(wnd);

            speech_queue_speak(L"\u590d\u5236\u6b4c\u8bcd\u9ed8\u8ba4\u8bbe\u7f6e\u5df2\u4fdd\u5b58", true);

            return TRUE;

        case IDC_ABOUT_GITHUB_REPO:

            open_url(wnd, L"https://github.com/zjmxczhy/foo_speaklyrics");

            return TRUE;

        case IDC_ABOUT_GITHUB_RELEASES:

            open_url(wnd, L"https://github.com/zjmxczhy/foo_speaklyrics/releases");

            return TRUE;

        case IDC_ABOUT_GITEE_RELEASES:

            open_url(wnd, L"https://gitee.com/zjmxczhy/foo_speaklyrics/releases");

            return TRUE;

        case IDC_TEST_SPEAK:

            speak_test_message();

            return TRUE;

        case IDOK:

            {
                bool restartRequested = save_dialog(wnd);

                EndDialog(wnd, IDOK);

                if (restartRequested) queue_foobar_restart();
            }

            return TRUE;

        case IDC_APPLY_SETTINGS:

            {
                bool restartRequested = save_dialog(wnd);

                if (restartRequested) {
                    EndDialog(wnd, IDOK);
                    queue_foobar_restart();
                    return TRUE;
                }
            }

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












