#pragma once
#include "stdafx.h"

struct lrc_line {
    int time_ms = 0;
    std::wstring text;
};

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
