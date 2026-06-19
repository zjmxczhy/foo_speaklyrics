#include "stdafx.h"
#include "config.h"
#include "speech_engine.h"

static const GUID guid_cfg_auto_speak = { 0x84c160de, 0xfd7a, 0x4dc9,{ 0x97, 0xf1, 0xa1, 0x52, 0xd6, 0x96, 0xc1, 0x10 } };
static const GUID guid_cfg_lead_ms = { 0x1c073b2c, 0xed7b, 0x4e41,{ 0xa2, 0x4f, 0xcb, 0x9, 0xda, 0xe4, 0xc2, 0x11 } };
static const GUID guid_cfg_missing_lrc_retry_ms = { 0x4db3f214, 0x9909, 0x47d0,{ 0x86, 0x38, 0x7d, 0x92, 0x7b, 0x88, 0x51, 0x3a } };
static const GUID guid_cfg_lyric_valid_ms = { 0xa7f36b55, 0x7744, 0x4e25,{ 0x8c, 0x71, 0x23, 0x39, 0x7f, 0x3e, 0xe8, 0x2d } };
static const GUID guid_cfg_temp_lrc_delete_delay_ms = { 0x5d5d1ad2, 0x4b5f, 0x4ad3,{ 0xa4, 0x18, 0x86, 0x8e, 0x9f, 0xd2, 0x40, 0x43 } };
static const GUID guid_cfg_lrc_file = { 0x65d151be, 0x8aac, 0x49c7,{ 0xbd, 0x69, 0x4c, 0x18, 0xf9, 0xf3, 0x11, 0x8c } };
static const GUID guid_cfg_lrc_folder = { 0xa27f64f3, 0x98ba, 0x445f,{ 0x92, 0xe0, 0x7f, 0x53, 0x15, 0xda, 0xce, 0x43 } };
static const GUID guid_cfg_temp_lrc_folder = { 0x4ff58d40, 0x357b, 0x4f13,{ 0xa5, 0xce, 0x78, 0x96, 0xf2, 0x5a, 0x43, 0xe2 } };
static const GUID guid_cfg_lyric_sources = { 0x9e0d41a4, 0x0d0d, 0x4f95,{ 0x8d, 0x66, 0x47, 0x0d, 0x4e, 0x2f, 0xa8, 0xb9 } };
static const GUID guid_cfg_lrc_encoding = { 0x3d1f6e32, 0x9b6a, 0x4dc3,{ 0x8a, 0xf4, 0x55, 0x5b, 0xb7, 0x2c, 0x01, 0x91 } };
static const GUID guid_cfg_download_to_lrc_folder = { 0x61e7711f, 0x4f2c, 0x4dd8,{ 0x9b, 0xe2, 0x23, 0xe7, 0x7a, 0x38, 0x51, 0x44 } };
static const GUID guid_cfg_use_screen_reader = { 0x7a1d0a52, 0x1b75, 0x4d1f,{ 0x91, 0x8d, 0x69, 0x14, 0x45, 0x12, 0x35, 0x77 } };
static const GUID guid_cfg_tts_voice_type = { 0x25e6539b, 0x63c8, 0x487b,{ 0x9d, 0x5f, 0x12, 0x4d, 0xde, 0x24, 0xa1, 0x0e } };
static const GUID guid_cfg_tts_voice_id = { 0xe6ab9ec3, 0x7995, 0x4882,{ 0x82, 0x4c, 0xae, 0x4f, 0x66, 0x9d, 0xc5, 0x92 } };
static const GUID guid_cfg_tts_rate = { 0xddd95cd8, 0x8cc0, 0x4f15,{ 0xb4, 0x55, 0x92, 0x35, 0x3c, 0x20, 0xfb, 0x69 } };

cfg_var_modern::cfg_bool cfg_auto_speak(guid_cfg_auto_speak, false);
cfg_var_modern::cfg_int cfg_lead_ms(guid_cfg_lead_ms, 0);
cfg_var_modern::cfg_int cfg_missing_lrc_retry_ms(guid_cfg_missing_lrc_retry_ms, 3000);
cfg_var_modern::cfg_int cfg_lyric_valid_ms(guid_cfg_lyric_valid_ms, 3000);
cfg_var_modern::cfg_int cfg_temp_lrc_delete_delay_ms(guid_cfg_temp_lrc_delete_delay_ms, 0);
cfg_var_modern::cfg_string cfg_lrc_file(guid_cfg_lrc_file, "");
cfg_var_modern::cfg_string cfg_lrc_folder(guid_cfg_lrc_folder, "");
cfg_var_modern::cfg_string cfg_temp_lrc_folder(guid_cfg_temp_lrc_folder, "%Temp%");
cfg_var_modern::cfg_string cfg_lyric_sources(guid_cfg_lyric_sources, "lrclib,qq1");
cfg_var_modern::cfg_string cfg_lrc_encoding(guid_cfg_lrc_encoding, "auto");
cfg_var_modern::cfg_bool cfg_download_to_lrc_folder(guid_cfg_download_to_lrc_folder, false);
cfg_var_modern::cfg_bool cfg_use_screen_reader(guid_cfg_use_screen_reader, true);
cfg_var_modern::cfg_string cfg_tts_voice_type(guid_cfg_tts_voice_type, "sapi5");
cfg_var_modern::cfg_string cfg_tts_voice_id(guid_cfg_tts_voice_id, "");
cfg_var_modern::cfg_int cfg_tts_rate(guid_cfg_tts_rate, 0);

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


