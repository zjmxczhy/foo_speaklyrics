#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/lyric_credit_filter.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

lyric_credit_filter_line make_line(int time_ms, const wchar_t* text) {
    return { time_ms, text };
}

void expect_body_start(
    const std::vector<lyric_credit_filter_line>& lines,
    const lyric_credit_filter_context& context,
    std::size_t expected_index,
    const wchar_t* expected_text) {
    const std::size_t actual = lyric_credit_filter::find_body_start(lines, context);
    assert(actual == expected_index);
    assert(actual < lines.size());
    assert(lines[actual].text == expected_text);
}

void test_furong_rain_keeps_first_real_lyric() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u8299\u84c9\u96e8 - \u5218\u73c2\u77e3"),
        make_line(9220, L"\u8bcd\uff1a\u5218\u73c2\u77e3/\u767e\u6155\u4e09\u77f3"),
        make_line(18450, L"\u66f2\uff1a\u5218\u73c2\u77e3/\u767e\u6155\u4e09\u77f3"),
        make_line(27670, L"\u7f16\u66f2\uff1a\u5218\u73c2\u77e3"),
        make_line(36900, L"\u85d5\u82b1\u9999 \u67d3\u6a90\u7259"),
        make_line(43360, L"\u60f9\u90a3\u8bd7\u4eba\u7eb5\u6b65\u968f\u5979"),
    };
    const lyric_credit_filter_context context = {
        L"\u8299\u84c9\u96e8", L"\u5218\u73c2\u77e3"
    };
    expect_body_start(lines, context, 4, L"\u85d5\u82b1\u9999 \u67d3\u6a90\u7259");
}

void test_full_moon_string_keeps_first_real_lyric() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u6708\u6ee1\u5f26 - \u5218\u73c2\u77e3"),
        make_line(8240, L"\u8bcd\uff1a\u5218\u73c2\u77e3"),
        make_line(16480, L"\u66f2\uff1a\u767e\u6155\u4e09\u77f3/\u5218\u73c2\u77e3"),
        make_line(24730, L"\u7f16\u66f2\uff1a\u767e\u6155\u4e09\u77f3"),
        make_line(32970, L"\u53e4\u5854\u65c1 \u62fe\u4e00\u5730\u91d1\u9ec4"),
        make_line(40070, L"\u96c1\u626b\u843d \u6ee1\u8179\u65e7\u971c"),
    };
    const lyric_credit_filter_context context = {
        L"\u6708\u6ee1\u5f26", L"\u5218\u73c2\u77e3"
    };
    expect_body_start(lines, context, 4, L"\u53e4\u5854\u65c1 \u62fe\u4e00\u5730\u91d1\u9ec4");
}

// The real intro from Yuanyang Xi must stop at its first sung line.
void test_yuanyang_intro_with_compact_title_artist_header() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5"),
        make_line(3340, L"\u8bcd\uff1a\u5bb6\u6d5a"),
        make_line(6680, L"\u66f2\uff1a\u5bb6\u6d5a/\u4e50\u91d1\u9707"),
        make_line(10020, L"OP/SP\uff1a\u660c\u79be\u6587\u5316"),
        make_line(13360, L"\u7279\u522b\u9e23\u8c22\uff1a\u5feb\u624b\u97f3\u4e50"),
        make_line(16710, L"\u4e00\u5e74\u56db\u5b63\u7684\u66f4\u66ff"),
        make_line(18630, L"\u7af9\u7bf1\u4e0b\u7684\u4e71\u82b1\u5f71"),
    };
    expect_body_start(lines, { L"\u9e33\u9e2f\u620f", L"\u5f20\u542b\u97f5" }, 5, L"\u4e00\u5e74\u56db\u5b63\u7684\u66f4\u66ff");
}

void test_compact_header_variants_match_title_and_artist() {
    const wchar_t* headers[] = {
        L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5", L"\u9e33\u9e2f\u620f -\u5f20\u542b\u97f5", L"\u9e33\u9e2f\u620f- \u5f20\u542b\u97f5",
        L"\u9e33\u9e2f\u620f\u2013\u5f20\u542b\u97f5", L"\u9e33\u9e2f\u620f\u2014\u5f20\u542b\u97f5", L"\u9e33\u9e2f\u620f\uff0d\u5f20\u542b\u97f5",
        L"\u5f20\u542b\u97f5-\u9e33\u9e2f\u620f", L"\u9e33\u9e2f\u620f - \u5f20\u542b\u97f5",
    };
    for (const auto* header : headers) {
        const std::vector<lyric_credit_filter_line> lines = {
            make_line(0, header),
            make_line(1000, L"OP/SP\uff1a\u67d0\u67d0"),
            make_line(2000, L"\u85d5\u82b1\u9999 \u67d3\u6a90\u7259"),
            make_line(3000, L"\u53e4\u5854\u65c1 \u62fe\u4e00\u5730\u91d1\u9ec4"),
        };
        expect_body_start(lines, { L"\u9e33\u9e2f\u620f", L"\u5f20\u542b\u97f5" }, 2, L"\u85d5\u82b1\u9999 \u67d3\u6a90\u7259");
    }
}

void test_compact_header_requires_matching_opposite_sides() {
    const lyric_credit_filter_context contexts[] = {
        {},
        { L"\u9e33\u9e2f\u620f", L"" },
        { L"", L"\u5f20\u542b\u97f5" },
        { L"\u9e33\u9e2f\u620f", L"\u5176\u4ed6\u6b4c\u624b" },
        { L"\u5176\u4ed6\u6b4c\u66f2", L"\u5f20\u542b\u97f5" },
        { L"\u9e33\u9e2f\u620f", L"\u9e33\u9e2f\u620f" },
    };
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5"),
        make_line(1000, L"\u8bcd\uff1a\u67d0\u67d0"),
        make_line(2000, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd"),
    };
    for (const auto& context : contexts) {
        expect_body_start(lines, context, 0, L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5");
    }
}

void test_compact_header_does_not_match_title_or_artist_substrings() {
    const wchar_t* headers[] = {
        L"\u9e33\u9e2f\u620f\u7684\u5f80\u4e8b-\u5f20\u542b\u97f5", L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5\u7684\u6b4c\u58f0",
        L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5-\u7b2c\u4e00\u53e5\u6b4c\u8bcd", L"\u9e33\u9e2f\u620f-",
        L"-\u5f20\u542b\u97f5", L"\u85d5\u82b1\u9999-\u67d3\u6a90\u7259",
    };
    for (const auto* header : headers) {
        const std::vector<lyric_credit_filter_line> lines = {
            make_line(0, header),
            make_line(1000, L"\u8bcd\uff1a\u67d0\u67d0"),
            make_line(2000, L"\u7b2c\u4e8c\u53e5\u6b4c\u8bcd"),
        };
        expect_body_start(lines, { L"\u9e33\u9e2f\u620f", L"\u5f20\u542b\u97f5" }, 0, header);
    }
}

void test_compact_header_supports_hyphens_inside_track_names() {
    const wchar_t* headers[] = { L"song-part-artist-name", L"artist-name-song-part" };
    for (const auto* header : headers) {
        const std::vector<lyric_credit_filter_line> lines = {
            make_line(0, header),
            make_line(1000, L"OP/SP: publisher"),
            make_line(2000, L"first lyric"),
        };
        expect_body_start(lines, { L"Song-Part", L"Artist-Name" }, 2, L"first lyric");
    }
}

void test_compact_header_preserves_extra_punctuation_and_word_boundaries() {
    const wchar_t* headers[] = {
        L"song-part-artist-name?", L"song-part-artist-name!",
        L"songpart-artist-name", L"song-part-artistname",
        L"s o n g-part-artist-name",
    };
    for (const auto* header : headers) {
        const std::vector<lyric_credit_filter_line> lines = {
            make_line(0, header),
            make_line(1000, L"OP/SP: publisher"),
            make_line(2000, L"first lyric"),
        };
        expect_body_start(lines, { L"Song-Part", L"Artist-Name" }, 0, header);
    }
}

void test_compact_header_cannot_move_an_accepted_body_boundary() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u7f16\u66f2\uff1a\u67d0\u67d0"),
        make_line(1000, L"\u53e4\u5854\u65c1-\u62fe\u4e00\u5730\u91d1\u9ec4"),
        make_line(2000, L"\u9e33\u9e2f\u620f-\u5f20\u542b\u97f5"),
        make_line(3000, L"OP/SP\uff1a\u67d0\u67d0"),
        make_line(4000, L"\u7b2c\u4e8c\u53e5\u6b4c\u8bcd"),
    };
    expect_body_start(lines, { L"\u9e33\u9e2f\u620f", L"\u5f20\u542b\u97f5" }, 1, L"\u53e4\u5854\u65c1-\u62fe\u4e00\u5730\u91d1\u9ec4");
}

void test_incomplete_credit_field_consumes_nearby_name_lines() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u4f5c\u8bcd\uff1a"),
        make_line(1000, L"\u5f20\u4e09"),
        make_line(2000, L"\u674e\u56db"),
        make_line(9000, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd"),
        make_line(14000, L"\u7b2c\u4e8c\u53e5\u6b4c\u8bcd"),
    };
    expect_body_start(lines, {}, 3, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd");
}

void test_complete_credit_keeps_immediate_short_lyric() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u7f16\u66f2\uff1a\u67d0\u67d0"),
        make_line(1000, L"\u554a"),
        make_line(2000, L"\u8fd8\u5728\u7b49\u4f60"),
    };
    expect_body_start(lines, {}, 1, L"\u554a");
}

void test_generic_and_known_credit_fields_are_filtered() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u4e50\u56681\uff1a\u67d0\u67d0"),
        make_line(1000, L"\u6df7\u97f3\uff1a\u67d0\u67d0"),
        make_line(2000, L"\u5f55\u97f3\uff1a\u67d0\u67d0"),
        make_line(8000, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd"),
    };
    expect_body_start(lines, {}, 3, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd");
}

void test_no_credit_block_keeps_every_line() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u85d5\u82b1\u9999 \u67d3\u6a90\u7259"),
        make_line(6000, L"\u60f9\u90a3\u8bd7\u4eba\u7eb5\u6b65\u968f\u5979"),
    };
    assert(lyric_credit_filter::find_body_start(lines, {}) == 0);
}

void test_credit_words_inside_body_do_not_move_boundary() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u4f5c\u8bcd\uff1a\u67d0\u67d0"),
        make_line(5000, L"\u7b2c\u4e00\u884c\u6b4c\u8bcd"),
        make_line(10000, L"\u53d1\u884c \u5728\u65f6\u5149\u91cc"),
        make_line(15000, L"\u548c\u58f0 \u4ece\u8fdc\u65b9\u4f20\u6765"),
        make_line(20000, L"\u8bbe\u8ba1 \u4e00\u573a\u68a6"),
    };
    expect_body_start(lines, {}, 1, L"\u7b2c\u4e00\u884c\u6b4c\u8bcd");
}

void test_distant_short_line_after_incomplete_field_is_preserved() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u4f5c\u8bcd\uff1a"),
        make_line(12000, L"\u98ce\u5439\u4e91\u52a8"),
        make_line(17000, L"\u5929\u4e0d\u52a8"),
    };
    expect_body_start(lines, {}, 1, L"\u98ce\u5439\u4e91\u52a8");
}

void test_later_credit_line_cannot_delete_an_accepted_body() {
    const std::vector<lyric_credit_filter_line> lines = {
        make_line(0, L"\u7b2c\u4e00\u53e5\u6b4c\u8bcd"),
        make_line(5000, L"\u4f5c\u8bcd\uff1a\u67d0\u67d0"),
        make_line(10000, L"\u7b2c\u4e8c\u53e5\u6b4c\u8bcd"),
    };
    assert(lyric_credit_filter::find_body_start(lines, {}) == 0);
}

}

int main() {
    test_furong_rain_keeps_first_real_lyric();
    test_full_moon_string_keeps_first_real_lyric();
    test_yuanyang_intro_with_compact_title_artist_header();
    test_compact_header_variants_match_title_and_artist();
    test_compact_header_requires_matching_opposite_sides();
    test_compact_header_does_not_match_title_or_artist_substrings();
    test_compact_header_supports_hyphens_inside_track_names();
    test_compact_header_preserves_extra_punctuation_and_word_boundaries();
    test_compact_header_cannot_move_an_accepted_body_boundary();
    test_incomplete_credit_field_consumes_nearby_name_lines();
    test_complete_credit_keeps_immediate_short_lyric();
    test_generic_and_known_credit_fields_are_filtered();
    test_no_credit_block_keeps_every_line();
    test_credit_words_inside_body_do_not_move_boundary();
    test_distant_short_line_after_incomplete_field_is_preserved();
    test_later_credit_line_cannot_delete_an_accepted_body();
    std::cout << "lyric_credit_filter tests passed\n";
    return 0;
}
