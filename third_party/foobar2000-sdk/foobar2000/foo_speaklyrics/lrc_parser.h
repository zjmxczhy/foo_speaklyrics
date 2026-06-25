#pragma once
#include "stdafx.h"

struct lrc_line {
    int time_ms = 0;
    std::wstring text;
};

struct lrc_encoding_info {
    const char* id;
    const wchar_t* name;
};

const lrc_encoding_info* lrc_get_encoding_options(size_t& count);
int lrc_find_encoding_index(const char* id);
std::wstring lrc_read_text_file(const std::wstring& path, std::string* detected_encoding_id, pfc::string8& error);
std::wstring lrc_read_text_file_auto(const std::wstring& path, std::string* detected_encoding_id, pfc::string8& error);
bool lrc_write_text_file(const std::wstring& path, const std::wstring& text, const char* encoding_id, pfc::string8& error);

class lrc_document {
public:
    bool load(const std::wstring& path, pfc::string8& error);
    void clear();
    bool empty() const { return m_lines.empty(); }
    int find_index_for_time(int ms) const;
    const lrc_line* get(size_t index) const;
    size_t count() const { return m_lines.size(); }
    const std::wstring& path() const { return m_path; }
private:
    std::vector<lrc_line> m_lines;
    std::wstring m_path;
};
