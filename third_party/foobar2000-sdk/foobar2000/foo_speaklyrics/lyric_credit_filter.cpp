#include "lyric_credit_filter.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>

namespace {

struct credit_field {
    const wchar_t* text;
    bool allow_space_separator;
};

struct credit_match {
    bool matched = false;
    bool incomplete = false;
};

bool is_credit_separator(wchar_t ch) {
    return ch == L':' || ch == L'\uff1a' || ch == L'\u2236' || ch == L'\ufe55' || ch == L'\ua789' ||
        ch == L'-' || ch == L'\u2013' || ch == L'\u2014' ||
        ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'\u00b7';
}

std::wstring normalized_credit_line(std::wstring text) {
    const wchar_t* whitespace = L" \t\r\n";
    const std::size_t start = text.find_first_not_of(whitespace);
    if (start == std::wstring::npos) return L"";
    const std::size_t end = text.find_last_not_of(whitespace);
    text = text.substr(start, end - start + 1);

    while (!text.empty()) {
        const wchar_t ch = text.front();
        if (ch != L'[' && ch != L'\u3010' && ch != L'(' && ch != L'\uff08' && ch != L'\u2022' &&
            ch != L'\u00b7' && ch != L'*' && ch != L'#') break;
        text.erase(text.begin());
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    }

    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return text;
}

credit_match match_credit_field(const std::wstring& text, const credit_field& field) {
    const std::size_t length = wcslen(field.text);
    if (text.size() < length || text.compare(0, length, field.text) != 0) return {};
    if (text.size() == length) return { true, true };

    std::size_t pos = length;
    if (!is_credit_separator(text[pos])) {
        if (!iswspace(text[pos])) return {};
        while (pos < text.size() && iswspace(text[pos])) ++pos;
        if (pos == text.size()) return { true, true };
        if (!is_credit_separator(text[pos])) {
            return field.allow_space_separator ? credit_match{ true, false } : credit_match{};
        }
    }

    while (pos < text.size() && (iswspace(text[pos]) || is_credit_separator(text[pos]))) ++pos;
    return { true, pos == text.size() };
}

credit_match match_known_credit_line(const std::wstring& raw_text) {
    const std::wstring text = normalized_credit_line(raw_text);
    if (text.empty()) return {};

    static const wchar_t* promotion_markers[] = {
        L"\u672c\u6b4c\u66f2\u7ffb\u5531\u7531", L"\u4e00\u952e\u7ffb\u5531", L"\u63d0\u4f9b\u751f\u4ea7\u80fd\u529b",
        L"ai\u7ffb\u5531", L"ai cover", L"\u672c\u97f3\u9891\u7531", L"\u661f\u66dc\u8ba1\u5212",
    };
    for (const wchar_t* marker : promotion_markers) {
        if (text.find(marker) != std::wstring::npos) return { true, false };
    }

    static const credit_field fields[] = {
        { L"\u97f3\u4e50\u5236\u4f5c\u4eba", true }, { L"\u97f3\u4e50\u603b\u76d1", true }, { L"\u6bcd\u5e26\u5de5\u7a0b\u5e08", true },
        { L"\u548c\u58f0\u7f16\u5199", true }, { L"\u4eba\u58f0\u7f16\u8f91", true }, { L"\u4e50\u5668\u5f55\u5236", true }, { L"\u6df7\u97f3\u5de5\u7a0b\u5e08", true },
        { L"\u5f55\u97f3\u5de5\u7a0b\u5e08", true }, { L"\u5f55\u97f3\u5e08", true }, { L"\u6df7\u97f3\u5e08", true }, { L"\u51fa\u54c1\u4eba", true },
        { L"\u4f5c\u8bcd", true }, { L"\u4f5c\u66f2", true }, { L"\u7f16\u66f2", true }, { L"\u6f14\u5531", true }, { L"\u539f\u5531", true },
        { L"\u6b4c\u624b", true }, { L"\u827a\u672f\u5bb6", true }, { L"\u5236\u4f5c\u4eba", true }, { L"\u76d1\u5236", true }, { L"\u7b56\u5212", true },
        { L"\u7edf\u7b79", true }, { L"\u5f55\u97f3", true }, { L"\u6df7\u97f3", true }, { L"\u6bcd\u5e26", true }, { L"\u5409\u4ed6", true },
        { L"\u8d1d\u65af", true }, { L"\u9f13", true }, { L"\u952e\u76d8", true }, { L"\u94a2\u7434", true }, { L"\u548c\u58f0", true },
        { L"\u914d\u5531", true }, { L"\u5f26\u4e50", true }, { L"\u5236\u8c31", true }, { L"\u5f55\u97f3\u68da", true }, { L"\u6df7\u97f3\u5ba4", true },
        { L"\u51fa\u54c1", true }, { L"\u53d1\u884c", true }, { L"\u5ba3\u4f20", true }, { L"\u6b4c\u66f2", true }, { L"\u6b4c\u540d", true },
        { L"\u66f2\u540d", true }, { L"\u4e13\u8f91", true }, { L"\u8bcd", false }, { L"\u66f2", false },
        { L"executive producer", true }, { L"recording engineer", true }, { L"mastering engineer", true },
        { L"mixing engineer", true }, { L"music director", true }, { L"vocal producer", true },
        { L"backing vocals", true }, { L"lyrics by", true }, { L"written by", true }, { L"composed by", true },
        { L"arranged by", true }, { L"produced by", true }, { L"recorded by", true }, { L"mixed by", true },
        { L"mastered by", true }, { L"published by", true }, { L"lyricist", true }, { L"composer", true },
        { L"arranger", true }, { L"producer", true }, { L"lyrics", true }, { L"singer", true },
        { L"artist", true }, { L"vocals", true }, { L"vocal", true }, { L"guitar", true }, { L"bass", true },
        { L"drums", true }, { L"keyboard", true }, { L"piano", true }, { L"strings", true }, { L"harmony", true },
        { L"publisher", true }, { L"copyright", true }, { L"label", true }, { L"title", true }, { L"album", true },
        { L"op", false }, { L"sp", false },
    };

    for (const auto& field : fields) {
        const credit_match match = match_credit_field(text, field);
        if (match.matched) return match;
    }

    static const wchar_t* declarations[] = {
        L"\u672a\u7ecf\u8457\u4f5c\u6743\u4eba\u8bb8\u53ef", L"\u672a\u7ecf\u8bb8\u53ef", L"\u7248\u6743\u6240\u6709", L"all rights reserved",
    };
    for (const wchar_t* declaration : declarations) {
        if (text.rfind(declaration, 0) == 0) return { true, false };
    }
    if (text.front() == L'\u00a9' || text.front() == L'\u2117') return { true, false };
    return {};
}

credit_match match_generic_credit_field_line(const std::wstring& raw_text) {
    const std::wstring text = normalized_credit_line(raw_text);
    if (text.empty()) return {};

    constexpr std::size_t maximum_credit_label_length = 64;
    const std::size_t colon = text.find_first_of(L":\uff1a\u2236\ufe55\ua789");
    if (colon == std::wstring::npos || colon == 0 || colon > maximum_credit_label_length) return {};

    std::size_t label_start = 0;
    while (label_start < colon && iswspace(text[label_start])) ++label_start;
    std::size_t label_end = colon;
    while (label_end > label_start && iswspace(text[label_end - 1])) --label_end;
    if (label_start == label_end) return {};

    for (std::size_t i = label_start; i < label_end; ++i) {
        const wchar_t ch = text[i];
        if (iswalnum(ch) || iswspace(ch) || ch > 127 || ch == L'&' || ch == L'/' || ch == L'\\' ||
            ch == L'.' || ch == L'+' || ch == L'-') continue;
        return {};
    }

    std::size_t value_start = colon + 1;
    while (value_start < text.size() && iswspace(text[value_start])) ++value_start;
    return { true, value_start == text.size() };
}

bool looks_like_unlabeled_artist_list(const std::wstring& raw_text) {
    const std::wstring text = normalized_credit_line(raw_text);
    if (text.empty() || text.size() > 160 || text.find(L"://") != std::wstring::npos) return false;

    std::size_t separator_count = 0;
    std::size_t nonempty_part_count = 0;
    std::size_t part_start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = i == text.size();
        const bool at_separator = !at_end && (text[i] == L'/' || text[i] == L'\uff0f' || text[i] == L'\u3001');
        if (!at_end && !at_separator) continue;

        std::size_t begin = part_start;
        std::size_t end = i;
        while (begin < end && iswspace(text[begin])) ++begin;
        while (end > begin && iswspace(text[end - 1])) --end;
        if (end > begin) {
            if (end - begin > 48) return false;
            ++nonempty_part_count;
        }
        if (at_separator) ++separator_count;
        part_start = i + 1;
    }

    return separator_count >= 2 && nonempty_part_count >= 3;
}

std::wstring normalized_header_identity(const std::wstring& raw_text) {
    std::wstring text;
    text.reserve(raw_text.size());
    for (const wchar_t ch : raw_text) {
        if (iswalnum(ch) || ch > 127) text.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return text;
}

bool same_header_identity(const std::wstring& first, const std::wstring& second) {
    const std::wstring normalized_first = normalized_header_identity(first);
    const std::wstring normalized_second = normalized_header_identity(second);
    return !normalized_first.empty() && normalized_first == normalized_second;
}

bool split_spaced_title_artist_header(const std::wstring& raw_text, std::wstring& left, std::wstring& right) {
    const std::wstring text = normalized_credit_line(raw_text);
    static const wchar_t separators[] = { L'-', L'\u2013', L'\u2014', L'\uff0d' };
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        if (std::find(std::begin(separators), std::end(separators), text[i]) == std::end(separators)) continue;
        if (!iswspace(text[i - 1]) || !iswspace(text[i + 1])) continue;

        left = normalized_credit_line(text.substr(0, i));
        right = normalized_credit_line(text.substr(i + 1));
        if (!left.empty() && !right.empty()) return true;
    }
    return false;
}

bool looks_like_leading_title_artist_header(
    const std::vector<lyric_credit_filter_line>& lines,
    std::size_t index,
    const lyric_credit_filter_context& context) {
    if (index >= lines.size() || index >= 5) return false;

    std::wstring left;
    std::wstring right;
    if (!split_spaced_title_artist_header(lines[index].text, left, right)) return false;

    const bool title_matches = same_header_identity(left, context.title) || same_header_identity(right, context.title);
    if (!title_matches) return false;

    const bool artist_matches = same_header_identity(left, context.artist) || same_header_identity(right, context.artist);
    if (!context.artist.empty() && artist_matches) return true;

    const bool starts_at_beginning = lines[index].time_ms <= 3000;
    const bool long_gap_after_header = index + 1 < lines.size() &&
        lines[index + 1].time_ms - lines[index].time_ms >= 8000;
    if (context.artist.empty() && starts_at_beginning && long_gap_after_header) return true;

    const std::size_t last_look_ahead = (std::min)(lines.size(), index + 4);
    for (std::size_t next = index + 1; next < last_look_ahead; ++next) {
        if (match_known_credit_line(lines[next].text).matched ||
            match_generic_credit_field_line(lines[next].text).matched) return true;
    }
    return false;
}

template <std::size_t Count>
bool contains_any_marker(const std::wstring& text, const wchar_t* const (&markers)[Count]) {
    for (const wchar_t* marker : markers) {
        if (text.find(marker) != std::wstring::npos) return true;
    }
    return false;
}

bool looks_like_leading_banner_line(const std::wstring& raw_text) {
    const std::wstring text = normalized_credit_line(raw_text);
    if (text.empty() || text.size() > 160) return false;
    if (text.find(L"://") != std::wstring::npos || text.find(L"www.") != std::wstring::npos) return true;

    static const wchar_t* platform_markers[] = {
        L"\u9177\u72d7", L"qq\u97f3\u4e50", L"\u7f51\u6613\u4e91", L"\u817e\u8baf\u97f3\u4e50", L"\u62d6\u97f3", L"\u5feb\u624b",
        L"bilibili", L"\u54aa\u5495\u97f3\u4e50", L"\u5343\u5343\u97f3\u4e50", L"\u97f3\u4e50\u4eba\u5e73\u53f0",
    };
    static const wchar_t* campaign_markers[] = {
        L"\u8ba1\u5212", L"\u4f01\u5212", L"\u9879\u76ee", L"\u72ec\u5bb6\u5448\u73b0", L"\u8054\u5408\u5448\u73b0", L"\u8363\u8a89\u5448\u73b0",
        L"\u5b98\u65b9\u9996\u53d1", L"\u7279\u522b\u9e23\u8c22", L"\u63a8\u5e7f", L"music project",
    };
    static const wchar_t* organization_markers[] = {
        L"\u6709\u9650\u516c\u53f8", L"\u6587\u5316\u4f20\u5a92", L"\u97f3\u4e50\u5382\u724c", L"\u97f3\u4e50\u5de5\u4f5c\u5ba4", L"studio presents",
    };

    const bool has_platform = contains_any_marker(text, platform_markers);
    const bool has_campaign = contains_any_marker(text, campaign_markers);
    const bool has_organization = contains_any_marker(text, organization_markers);
    if (has_organization) return true;
    if (has_platform && text.size() <= 80) return true;
    if (has_campaign && text.size() <= 60) return true;

    if (text.size() >= 2) {
        const wchar_t first = text.front();
        const wchar_t last = text.back();
        const bool fully_wrapped =
            (first == L'\u300c' && last == L'\u300d') || (first == L'\u300e' && last == L'\u300f') ||
            (first == L'\u3010' && last == L'\u3011') || (first == L'\u300a' && last == L'\u300b');
        if (fully_wrapped && (has_platform || has_campaign || has_organization)) return true;
    }
    return false;
}

bool is_cjk_name_character(wchar_t ch) {
    return (ch >= 0x3400 && ch <= 0x9fff) || (ch >= 0xf900 && ch <= 0xfaff);
}

bool is_name_list_separator(wchar_t ch) {
    return ch == L'/' || ch == L'\uff0f' || ch == L'\u3001' || ch == L',' || ch == L'\uff0c' ||
        ch == L'&' || ch == L'+' || ch == L'\uff0b' || ch == L';' || ch == L'\uff1b';
}

bool looks_like_name_token(const std::wstring& raw_token) {
    const std::wstring token = normalized_credit_line(raw_token);
    if (token.empty() || token.size() > 40) return false;

    std::size_t cjk_count = 0;
    std::size_t latin_count = 0;
    std::size_t word_count = 0;
    bool in_latin_word = false;
    bool has_space = false;
    bool has_middle_dot = false;

    for (const wchar_t ch : token) {
        if (is_cjk_name_character(ch)) {
            ++cjk_count;
            in_latin_word = false;
            continue;
        }
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) {
            ++latin_count;
            if (!in_latin_word) ++word_count;
            in_latin_word = true;
            continue;
        }
        if (ch >= L'0' && ch <= L'9') {
            in_latin_word = true;
            continue;
        }
        if (iswspace(ch)) {
            has_space = true;
            in_latin_word = false;
            continue;
        }
        if (ch == L'\u00b7' || ch == L'\u30fb') {
            has_middle_dot = true;
            in_latin_word = false;
            continue;
        }
        if (ch == L'\'' || ch == L'-' || ch == L'_' || ch == L'.') {
            in_latin_word = false;
            continue;
        }
        return false;
    }

    if (cjk_count > 0 && latin_count == 0) {
        if (has_space) return false;
        if (has_middle_dot) return cjk_count >= 2 && cjk_count <= 12;
        return cjk_count >= 2 && cjk_count <= 4;
    }
    if (cjk_count > 0 && latin_count > 0) return !has_space && token.size() <= 16;
    return latin_count > 0 && word_count >= 1 && word_count <= 4;
}

bool looks_like_credit_value_line(const std::wstring& raw_text) {
    const std::wstring text = normalized_credit_line(raw_text);
    if (text.empty() || text.size() > 96 || text.find(L"://") != std::wstring::npos) return false;
    if (match_known_credit_line(text).matched || match_generic_credit_field_line(text).matched ||
        looks_like_leading_banner_line(text)) return false;
    if (text.find_first_of(L"\u3002\uff01\uff1f!?\uff1b;\u201c\u201d\u300a\u300b") != std::wstring::npos) return false;

    std::size_t token_start = 0;
    std::size_t token_count = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool at_end = i == text.size();
        if (!at_end && !is_name_list_separator(text[i])) continue;

        if (!looks_like_name_token(text.substr(token_start, i - token_start))) return false;
        ++token_count;
        token_start = i + 1;
    }
    return token_count > 0;
}

bool continuation_is_close(const lyric_credit_filter_line& previous, const lyric_credit_filter_line& current) {
    constexpr int maximum_gap_ms = 5000;
    if (current.time_ms < previous.time_ms) return true;
    return current.time_ms - previous.time_ms <= maximum_gap_ms;
}

bool generic_field_is_accepted(
    const std::vector<lyric_credit_filter_line>& lines,
    std::size_t index,
    bool credit_block_started) {
    const credit_match current = match_generic_credit_field_line(lines[index].text);
    if (!current.matched) return false;
    if (index < 5 || credit_block_started) return true;
    return index + 1 < lines.size() && index + 1 < 20 &&
        match_generic_credit_field_line(lines[index + 1].text).matched;
}

}

namespace lyric_credit_filter {

std::size_t find_body_start(
    const std::vector<lyric_credit_filter_line>& lines,
    const lyric_credit_filter_context& context) {
    if (lines.empty()) return 0;

    constexpr std::size_t maximum_lines_to_discover_block = 40;
    constexpr std::size_t maximum_continuation_lines = 4;
    bool credit_block_started = false;
    bool waiting_for_field_value = false;
    std::size_t continuation_lines = 0;
    std::size_t cut = 0;
    lyric_credit_filter_line previous_credit_or_value;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!credit_block_started && i >= maximum_lines_to_discover_block) return 0;

        const std::wstring normalized = normalized_credit_line(lines[i].text);
        if (normalized.empty()) {
            if (credit_block_started) cut = i + 1;
            continue;
        }

        const credit_match known_match = match_known_credit_line(normalized);
        const credit_match generic_match = match_generic_credit_field_line(normalized);
        const bool title_artist_header = looks_like_leading_title_artist_header(lines, i, context);
        const bool banner = looks_like_leading_banner_line(normalized);
        const bool unlabeled_artist_list = i < 10 && looks_like_unlabeled_artist_list(normalized);
        const bool accepted_generic = generic_match.matched &&
            generic_field_is_accepted(lines, i, credit_block_started);

        if (title_artist_header || banner || known_match.matched || unlabeled_artist_list || accepted_generic) {
            credit_block_started = true;
            cut = i + 1;
            waiting_for_field_value = known_match.incomplete || (accepted_generic && generic_match.incomplete);
            continuation_lines = 0;
            previous_credit_or_value = lines[i];
            continue;
        }

        if (!credit_block_started) {
            // Once an ordinary line appears, later credit-like words cannot
            // retroactively turn already accepted lyrics into metadata.
            return 0;
        }

        if (waiting_for_field_value && continuation_lines < maximum_continuation_lines &&
            looks_like_credit_value_line(normalized) &&
            continuation_is_close(previous_credit_or_value, lines[i])) {
            cut = i + 1;
            ++continuation_lines;
            previous_credit_or_value = lines[i];
            continue;
        }

        // A complete credit field is followed immediately by the lyric body.
        // An incomplete field only consumes strict, nearby name-like values.
        return i;
    }

    return credit_block_started ? cut : 0;
}

}
