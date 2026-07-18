#include "stdafx.h"
#include "config.h"
#include "speech_engine.h"
#include "speaklyrics_log.h"

static const GUID guid_cfg_auto_speak = { 0x84c160de, 0xfd7a, 0x4dc9,{ 0x97, 0xf1, 0xa1, 0x52, 0xd6, 0x96, 0xc1, 0x10 } };
static const GUID guid_cfg_announce_track_on_change = { 0x0f49fb33, 0x6d57, 0x4b78,{ 0x8d, 0x22, 0x82, 0x29, 0x57, 0xdb, 0x5d, 0xa1 } };
static const GUID guid_cfg_announce_track_format = { 0x59440367, 0x7f9a, 0x4057,{ 0x84, 0x76, 0x7c, 0x44, 0x77, 0x8a, 0xa4, 0x26 } };
static const GUID guid_cfg_announce_track_delay_ms = { 0xb1abbb64, 0x6f32, 0x4f2b,{ 0x92, 0x1a, 0x77, 0x7e, 0x42, 0xa4, 0x29, 0xa0 } };
static const GUID guid_cfg_lead_ms = { 0x1c073b2c, 0xed7b, 0x4e41,{ 0xa2, 0x4f, 0xcb, 0x9, 0xda, 0xe4, 0xc2, 0x11 } };
static const GUID guid_cfg_missing_lrc_retry_ms = { 0x4db3f214, 0x9909, 0x47d0,{ 0x86, 0x38, 0x7d, 0x92, 0x7b, 0x88, 0x51, 0x3a } };
static const GUID guid_cfg_lyric_valid_ms = { 0xa7f36b55, 0x7744, 0x4e25,{ 0x8c, 0x71, 0x23, 0x39, 0x7f, 0x3e, 0xe8, 0x2d } };
static const GUID guid_cfg_temp_lrc_delete_delay_ms = { 0x5d5d1ad2, 0x4b5f, 0x4ad3,{ 0xa4, 0x18, 0x86, 0x8e, 0x9f, 0xd2, 0x40, 0x43 } };
static const GUID guid_cfg_lrc_file = { 0x65d151be, 0x8aac, 0x49c7,{ 0xbd, 0x69, 0x4c, 0x18, 0xf9, 0xf3, 0x11, 0x8c } };
static const GUID guid_cfg_lrc_folder = { 0xa27f64f3, 0x98ba, 0x445f,{ 0x92, 0xe0, 0x7f, 0x53, 0x15, 0xda, 0xce, 0x43 } };
static const GUID guid_cfg_temp_lrc_folder = { 0x4ff58d40, 0x357b, 0x4f13,{ 0xa5, 0xce, 0x78, 0x96, 0xf2, 0x5a, 0x43, 0xe2 } };
static const GUID guid_cfg_lyric_sources = { 0x9e0d41a4, 0x0d0d, 0x4f95,{ 0x8d, 0x66, 0x47, 0x0d, 0x4e, 0x2f, 0xa8, 0xb9 } };
static const GUID guid_cfg_lrc_encoding = { 0x3d1f6e32, 0x9b6a, 0x4dc3,{ 0x8a, 0xf4, 0x55, 0x5b, 0xb7, 0x2c, 0x01, 0x91 } };
static const GUID guid_cfg_lyric_speak_mode = { 0x8f90f635, 0xc663, 0x4aa1,{ 0xa6, 0x37, 0x8f, 0xf4, 0x85, 0x39, 0x7b, 0x61 } };
static const GUID guid_cfg_copy_mode = { 0xd471f286, 0xe6f3, 0x42f4,{ 0x92, 0xed, 0x54, 0x8a, 0x19, 0x2f, 0xc8, 0x08 } };
static const GUID guid_cfg_copy_split_separators = { 0xfdf9fb32, 0x87d7, 0x4f9b,{ 0xb0, 0x9f, 0xc1, 0xc4, 0xc0, 0x4d, 0x7f, 0x99 } };
static const GUID guid_cfg_copy_filter_leading_credits = { 0x9957fe13, 0x192d, 0x49fb,{ 0x99, 0x0d, 0x3f, 0x36, 0x79, 0xf1, 0x28, 0xe7 } };
static const GUID guid_cfg_use_screen_reader = { 0x7a1d0a52, 0x1b75, 0x4d1f,{ 0x91, 0x8d, 0x69, 0x14, 0x45, 0x12, 0x35, 0x77 } };
static const GUID guid_cfg_screen_reader_channel_target = { 0xfcd6d10f, 0x5430, 0x4281,{ 0xa3, 0xac, 0x66, 0xd1, 0x0b, 0x49, 0xe1, 0x15 } };
static const GUID guid_cfg_boy_channel_mode = { 0x391258d0, 0x11ee, 0x41ed,{ 0xb0, 0x14, 0xbc, 0x0f, 0xdb, 0x2b, 0x67, 0x89 } };
static const GUID guid_cfg_boy_channel_name = { 0x65cf5657, 0x9450, 0x4d23,{ 0x92, 0xfe, 0x76, 0xc7, 0xa2, 0xa1, 0x7b, 0xfd } };
static const GUID guid_cfg_zdsr_channel_mode = { 0x4d8bdb98, 0x99ba, 0x4a6a,{ 0xaa, 0xd7, 0x6e, 0x1d, 0xb2, 0x47, 0x40, 0x3c } };
static const GUID guid_cfg_zdsr_channel_name = { 0x11821788, 0x3f1d, 0x4315,{ 0x8b, 0xa4, 0xb2, 0x06, 0xc2, 0x23, 0xb1, 0x92 } };
static const GUID guid_cfg_tts_voice_type = { 0x25e6539b, 0x63c8, 0x487b,{ 0x9d, 0x5f, 0x12, 0x4d, 0xde, 0x24, 0xa1, 0x0e } };
static const GUID guid_cfg_tts_voice_id = { 0xe6ab9ec3, 0x7995, 0x4882,{ 0x82, 0x4c, 0xae, 0x4f, 0x66, 0x9d, 0xc5, 0x92 } };
static const GUID guid_cfg_tts_rate = { 0xddd95cd8, 0x8cc0, 0x4f15,{ 0xb4, 0x55, 0x92, 0x35, 0x3c, 0x20, 0xfb, 0x69 } };

cfg_bool cfg_auto_speak(guid_cfg_auto_speak, false);
cfg_bool cfg_announce_track_on_change(guid_cfg_announce_track_on_change, false);
cfg_string cfg_announce_track_format(guid_cfg_announce_track_format, "title_artist");
cfg_int cfg_announce_track_delay_ms(guid_cfg_announce_track_delay_ms, 0);
cfg_int cfg_lead_ms(guid_cfg_lead_ms, 0);
cfg_int cfg_missing_lrc_retry_ms(guid_cfg_missing_lrc_retry_ms, 3000);
cfg_int cfg_lyric_valid_ms(guid_cfg_lyric_valid_ms, 3000);
cfg_int cfg_temp_lrc_delete_delay_ms(guid_cfg_temp_lrc_delete_delay_ms, 0);
cfg_string cfg_lrc_file(guid_cfg_lrc_file, "");
cfg_string cfg_lrc_folder(guid_cfg_lrc_folder, "");
cfg_string cfg_temp_lrc_folder(guid_cfg_temp_lrc_folder, "%Temp%");
cfg_string cfg_lyric_sources(guid_cfg_lyric_sources, "lrclib,qq1");
cfg_string cfg_lrc_encoding(guid_cfg_lrc_encoding, "auto");
cfg_string cfg_lyric_speak_mode(guid_cfg_lyric_speak_mode, "timestamp");
cfg_string cfg_copy_mode(guid_cfg_copy_mode, "ask");
cfg_string cfg_copy_split_separators(guid_cfg_copy_split_separators, "");
cfg_bool cfg_copy_filter_leading_credits(guid_cfg_copy_filter_leading_credits, false);
cfg_bool cfg_use_screen_reader(guid_cfg_use_screen_reader, true);
cfg_string cfg_screen_reader_channel_target(guid_cfg_screen_reader_channel_target, "auto");
cfg_string cfg_boy_channel_mode(guid_cfg_boy_channel_mode, "named");
cfg_string cfg_boy_channel_name(guid_cfg_boy_channel_name, "\xE6\x9C\x97\xE8\xAF\xBB\xE6\xAD\x8C\xE8\xAF\x8D\xE9\x80\x9A\xE9\x81\x93");
cfg_string cfg_zdsr_channel_mode(guid_cfg_zdsr_channel_mode, "named");
cfg_string cfg_zdsr_channel_name(guid_cfg_zdsr_channel_name, "\xE6\x9C\x97\xE8\xAF\xBB\xE6\xAD\x8C\xE8\xAF\x8D\xE9\x80\x9A\xE9\x81\x93");
cfg_string cfg_tts_voice_type(guid_cfg_tts_voice_type, "sapi5");
cfg_string cfg_tts_voice_id(guid_cfg_tts_voice_id, "");
cfg_int cfg_tts_rate(guid_cfg_tts_rate, 0);

std::wstring expand_environment_path(const std::wstring& path) {
    if (path.empty()) return path;
    DWORD needed = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (needed == 0) return path;
    std::wstring expanded(static_cast<size_t>(needed), L'\0');
    DWORD written = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), needed);
    if (written == 0 || written > needed) return path;
    if (written > 0 && expanded[written - 1] == L'\0') --written;
    expanded.resize(static_cast<size_t>(written));
    return expanded;
}

namespace {

std::wstring channel_cfg_to_wide(cfg_string& value) {
    pfc::string8 utf8 = value.get();
    return pfc::stringcvt::string_wide_from_utf8(utf8).get_ptr();
}

std::wstring sanitize_channel_name(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return ch == L'\r' || ch == L'\n' || ch < L' ';
    }), value.end());
    while (!value.empty() && value.front() <= L' ') value.erase(value.begin());
    while (!value.empty() && value.back() <= L' ') value.pop_back();
    if (value.size() > 64) value.resize(64);
    if (value.empty()) value = L"\u6717\u8bfb\u6b4c\u8bcd\u901a\u9053";
    return value;
}

bool channel_mode_is(cfg_string& mode, const char* expected) {
    pfc::string8 value = mode.get();
    return _stricmp(value.get_ptr(), expected) == 0;
}

bool encode_channel_config(const std::wstring& text, UINT codePage, std::vector<char>& output) {
    DWORD flags = codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
    BOOL usedDefault = FALSE;
    BOOL* usedDefaultPtr = codePage == CP_UTF8 ? nullptr : &usedDefault;
    int required = WideCharToMultiByte(codePage, flags, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, usedDefaultPtr);
    if (required <= 0 || usedDefault) return false;
    output.resize(static_cast<size_t>(required));
    usedDefault = FALSE;
    int written = WideCharToMultiByte(codePage, flags, text.c_str(), static_cast<int>(text.size()), output.data(), required, nullptr, usedDefaultPtr);
    return written == required && !usedDefault;
}

bool write_channel_config_file(const std::wstring& path, const std::vector<char>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.size() <= MAXDWORD &&
        WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != FALSE &&
        written == data.size();
    CloseHandle(file);
    return ok;
}

std::wstring component_directory() {
    pfc::string8 path = core_api::get_my_full_path();
    std::wstring wide = pfc::stringcvt::string_wide_from_utf8(path).get_ptr();
    size_t slash = wide.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : wide.substr(0, slash);
}

}

bool write_screen_reader_channel_config_files() {
    std::wstring componentDir = component_directory();
    if (componentDir.empty()) return false;
    std::wstring tolkDir = componentDir + L"\\tolk";
    if (!CreateDirectoryW(tolkDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        speaklyrics_log_warning(L"\u65e0\u6cd5\u521b\u5efa\u8bfb\u5c4f\u901a\u9053\u914d\u7f6e\u76ee\u5f55\uff1a%s", tolkDir.c_str());
        return false;
    }

    const bool boyMain = channel_mode_is(cfg_boy_channel_mode, "main");
    const bool boyNamed = channel_mode_is(cfg_boy_channel_mode, "named");
    const bool zdsrMain = channel_mode_is(cfg_zdsr_channel_mode, "main");
    const bool zdsrNamed = channel_mode_is(cfg_zdsr_channel_mode, "named");
    std::wstring boyName = boyNamed ? sanitize_channel_name(channel_cfg_to_wide(cfg_boy_channel_name)) : L"";
    std::wstring zdsrName = zdsrNamed ? sanitize_channel_name(channel_cfg_to_wide(cfg_zdsr_channel_name)) : L"";

    std::wstring boyText = L"# foo_speaklyrics screen reader channel settings\r\n";
    boyText += L"speak.useSlave = ";
    boyText += boyMain ? L"0\r\n" : L"1\r\n";
    boyText += L"speak.slaveName = " + boyName + L"\r\n";
    boyText += L"speak.append = 0\r\n";
    boyText += L"speak.allowBreak = 1\r\n";
    boyText += L"anyKeyBreak = 0\r\n";
    boyText += L"logPath = \r\n";

    std::wstring zdsrText = L"[settings]\r\n";
    zdsrText += L"type=";
    zdsrText += zdsrMain ? L"0\r\n" : L"1\r\n";
    zdsrText += L"channelName=" + zdsrName + L"\r\n";
    zdsrText += L"keyDownInterrupt=\r\n";
    zdsrText += L"alwaysInterrupt=\r\n";

    std::vector<char> boyBytes;
    std::vector<char> zdsrBytes;
    if (!encode_channel_config(boyText, CP_UTF8, boyBytes) || !encode_channel_config(zdsrText, 936, zdsrBytes)) {
        speaklyrics_log_warning(L"\u8bfb\u5c4f\u901a\u9053\u540d\u79f0\u65e0\u6cd5\u4f7f\u7528\u6240\u9700\u7f16\u7801\u4fdd\u5b58\u3002");
        return false;
    }

    bool boyOk = write_channel_config_file(tolkDir + L"\\byctrl.conf", boyBytes);
    bool zdsrOk = write_channel_config_file(tolkDir + L"\\ZDSRAPI.ini", zdsrBytes);
    if (!boyOk || !zdsrOk) {
        speaklyrics_log_warning(L"\u65e0\u6cd5\u5199\u5165\u8bfb\u5c4f\u901a\u9053\u914d\u7f6e\uff1a%s", tolkDir.c_str());
    }
    return boyOk && zdsrOk;
}

void speak_test_text(const wchar_t* text) {
    if (!speech_speak(text && *text ? text : L"\u6717\u8bfb\u6b4c\u8bcd\u6d4b\u8bd5", true)) {
        pfc::string8 msg = pfc::stringcvt::string_utf8_from_wide(
            L"\u65e0\u6cd5\u8c03\u7528\u5f53\u524d\u9009\u62e9\u7684\u8bed\u97f3\u8f93\u51fa\u3002"
            L"\u5982\u679c\u4f7f\u7528\u5c4f\u5e55\u9605\u8bfb\u5668\uff0c\u8bf7\u786e\u8ba4\u4e89\u6e21\u3001NVDA \u7b49\u8bfb\u5c4f\u6b63\u5728\u8fd0\u884c\uff1b"
            L"\u5982\u679c\u4f7f\u7528 SAPI/OneCore\uff0c\u8bf7\u786e\u8ba4 Windows \u4e2d\u5df2\u5b89\u88c5\u53ef\u7528\u8bed\u97f3\u3002");
        pfc::string8 title = pfc::stringcvt::string_utf8_from_wide(L"\u6717\u8bfb\u6b4c\u8bcd");
        popup_message::g_show(msg.get_ptr(), title.get_ptr());
    }
}

void speak_test_message() {
    speak_test_text(L"\u6717\u8bfb\u6b4c\u8bcd\u6d4b\u8bd5");
}


