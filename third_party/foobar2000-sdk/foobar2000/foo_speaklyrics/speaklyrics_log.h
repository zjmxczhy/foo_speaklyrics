#pragma once

void speaklyrics_log_info(const wchar_t* format, ...);
void speaklyrics_log_warning(const wchar_t* format, ...);
void speaklyrics_log_error(const wchar_t* format, ...);
std::wstring speaklyrics_log_file_path();
