#pragma once
#include "stdafx.h"

extern cfg_bool cfg_auto_speak;
extern cfg_bool cfg_announce_track_on_change;
extern cfg_string cfg_announce_track_format;
extern cfg_int cfg_announce_track_delay_ms;
extern cfg_int cfg_lead_ms;
extern cfg_int cfg_missing_lrc_retry_ms;
extern cfg_int cfg_lyric_valid_ms;
extern cfg_int cfg_temp_lrc_delete_delay_ms;
extern cfg_string cfg_lrc_file;
extern cfg_string cfg_lrc_folder;
extern cfg_string cfg_temp_lrc_folder;
extern cfg_string cfg_lrc_encoding;
extern cfg_string cfg_lyric_speak_mode;
extern cfg_string cfg_lyric_sources;
extern cfg_string cfg_copy_mode;
extern cfg_string cfg_copy_split_separators;
extern cfg_bool cfg_copy_filter_leading_credits;
extern cfg_bool cfg_use_screen_reader;
extern cfg_string cfg_screen_reader_channel_target;
extern cfg_string cfg_boy_channel_mode;
extern cfg_string cfg_boy_channel_name;
extern cfg_string cfg_zdsr_channel_mode;
extern cfg_string cfg_zdsr_channel_name;
extern cfg_string cfg_tts_voice_type;
extern cfg_string cfg_tts_voice_id;
extern cfg_int cfg_tts_rate;

void show_settings_dialog(HWND parent);
bool browse_lrc_file(HWND parent, pfc::string8& out);
bool browse_lrc_folder(HWND parent, pfc::string8& out);
bool browse_temp_lrc_folder(HWND parent, pfc::string8& out);
void speak_test_message();
void speak_test_text(const wchar_t* text);
std::wstring expand_environment_path(const std::wstring& path);
bool write_screen_reader_channel_config_files();



