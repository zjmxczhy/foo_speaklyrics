#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct lyric_credit_filter_line {
    int time_ms = 0;
    std::wstring text;
};

struct lyric_credit_filter_context {
    std::wstring title;
    std::wstring artist;
};

namespace lyric_credit_filter {

std::size_t find_body_start(
    const std::vector<lyric_credit_filter_line>& lines,
    const lyric_credit_filter_context& context);

}
