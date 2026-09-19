#pragma once

void speaklyrics_log_info(const wchar_t* format, ...);
void speaklyrics_log_warning(const wchar_t* format, ...);
void speaklyrics_log_error(const wchar_t* format, ...);
std::wstring speaklyrics_log_file_path();
std::wstring speaklyrics_log_text_excerpt(const wchar_t* text, size_t maximumCharacters = 48);
uint64_t speaklyrics_log_text_hash(const wchar_t* text);
