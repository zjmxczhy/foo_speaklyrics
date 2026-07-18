#include "stdafx.h"

#include "config.h"
#include "lyrics_copy.h"
#include "playback.h"
#include "resource.h"
#include "speech_engine.h"

namespace {

lyric_copy_mode g_selected_dialog_mode = lyric_copy_mode::ask;
HWND g_copy_dialog_wnd = nullptr;

const lyric_copy_mode k_dialog_modes[] = {
    lyric_copy_mode::timestamps,
    lyric_copy_mode::plain,
    lyric_copy_mode::split,
};

void activate_copy_dialog() {
    if (!g_copy_dialog_wnd || !IsWindow(g_copy_dialog_wnd)) return;
    ShowWindow(g_copy_dialog_wnd, SW_SHOWNORMAL);
    BringWindowToTop(g_copy_dialog_wnd);
    SetForegroundWindow(g_copy_dialog_wnd);
    HWND list = GetDlgItem(g_copy_dialog_wnd, IDC_COPY_MODE_LIST);
    SetFocus(list ? list : g_copy_dialog_wnd);
}

std::wstring format_timestamp(int time_ms) {
    if (time_ms < 0) time_ms = 0;
    int minutes = time_ms / 60000;
    int seconds = (time_ms / 1000) % 60;
    int centiseconds = (time_ms % 1000) / 10;
    wchar_t buffer[32] = {};
    swprintf_s(buffer, L"[%02d:%02d.%02d]", minutes, seconds, centiseconds);
    return buffer;
}

bool contains_split_char(const std::wstring& separators, wchar_t ch) {
    return !separators.empty() && separators.find(ch) != std::wstring::npos;
}

bool is_line_break(wchar_t ch) {
    return ch == L'\r' || ch == L'\n';
}

std::vector<lyric_jump_item> current_lyrics() {
    return get_current_lyric_jump_items();
}

struct credit_field {
    const wchar_t* text;
    bool allow_space_separator;
};

bool is_credit_separator(wchar_t ch) {
    return ch == L':' || ch == L'\uff1a' || ch == L'\u2236' || ch == L'\ufe55' || ch == L'\ua789' ||
        ch == L'-' || ch == L'\u2013' || ch == L'\u2014' ||
        ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'\u00b7';
}

std::wstring normalized_credit_line(std::wstring text) {
    const wchar_t* whitespace = L" \t\r\n";
    size_t start = text.find_first_not_of(whitespace);
    if (start == std::wstring::npos) return L"";
    size_t end = text.find_last_not_of(whitespace);
    text = text.substr(start, end - start + 1);

    while (!text.empty()) {
        wchar_t ch = text.front();
        if (ch != L'[' && ch != L'\u3010' && ch != L'(' && ch != L'\uff08' && ch != L'\u2022' &&
            ch != L'\u00b7' && ch != L'*' && ch != L'#') break;
        text.erase(text.begin());
        while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    }

    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return text;
}

bool starts_with_credit_field(const std::wstring& text, const credit_field& field) {
    const size_t length = wcslen(field.text);
    if (text.size() < length || text.compare(0, length, field.text) != 0) return false;
    if (text.size() == length) return true;

    size_t pos = length;
    if (is_credit_separator(text[pos])) return true;
    if (!iswspace(text[pos])) return false;

    while (pos < text.size() && iswspace(text[pos])) ++pos;
    if (pos == text.size() || is_credit_separator(text[pos])) return true;
    return field.allow_space_separator;
}

bool is_leading_credit_line(const std::wstring& rawText) {
    const std::wstring text = normalized_credit_line(rawText);
    if (text.empty()) return false;

    static const wchar_t* promotionMarkers[] = {
        L"\u672c\u6b4c\u66f2\u7ffb\u5531\u7531", L"\u4e00\u952e\u7ffb\u5531", L"\u63d0\u4f9b\u751f\u4ea7\u80fd\u529b",
        L"ai\u7ffb\u5531", L"ai cover", L"\u672c\u97f3\u9891\u7531", L"\u661f\u66dc\u8ba1\u5212",
    };
    for (const wchar_t* marker : promotionMarkers) {
        if (text.find(marker) != std::wstring::npos) return true;
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
        if (starts_with_credit_field(text, field)) return true;
    }

    static const wchar_t* declarations[] = {
        L"\u672a\u7ecf\u8457\u4f5c\u6743\u4eba\u8bb8\u53ef", L"\u672a\u7ecf\u8bb8\u53ef", L"\u7248\u6743\u6240\u6709", L"all rights reserved",
    };
    for (const wchar_t* declaration : declarations) {
        if (text.rfind(declaration, 0) == 0) return true;
    }
    return text.front() == L'\u00a9' || text.front() == L'\u2117';
}

bool looks_like_unlabeled_artist_list(const std::wstring& rawText) {
    std::wstring text = normalized_credit_line(rawText);
    if (text.empty() || text.size() > 160 || text.find(L"://") != std::wstring::npos) return false;

    size_t separatorCount = 0;
    size_t nonemptyPartCount = 0;
    size_t partStart = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        const bool atEnd = i == text.size();
        const bool atSeparator = !atEnd && (text[i] == L'/' || text[i] == L'\uff0f' || text[i] == L'\u3001');
        if (!atEnd && !atSeparator) continue;

        size_t begin = partStart;
        size_t end = i;
        while (begin < end && iswspace(text[begin])) ++begin;
        while (end > begin && iswspace(text[end - 1])) --end;
        if (end > begin) {
            if (end - begin > 48) return false;
            ++nonemptyPartCount;
        }
        if (atSeparator) ++separatorCount;
        partStart = i + 1;
    }

    return separatorCount >= 2 && nonemptyPartCount >= 3;
}

bool looks_like_generic_credit_field_line(const std::wstring& rawText) {
    const std::wstring text = normalized_credit_line(rawText);
    if (text.empty()) return false;

    // Bilingual production labels can be considerably longer than their
    // Chinese-only form, for example "制作统筹Production Coordination".
    // This remains safe because generic fields are only accepted while the
    // leading credit block is being established or continued.
    constexpr size_t kMaximumCreditLabelLength = 64;
    const size_t colon = text.find_first_of(L":\uff1a\u2236\ufe55\ua789");
    if (colon == std::wstring::npos || colon == 0 || colon > kMaximumCreditLabelLength) return false;

    size_t valueStart = colon + 1;
    while (valueStart < text.size() && iswspace(text[valueStart])) ++valueStart;
    if (valueStart == text.size()) return false;

    size_t labelStart = 0;
    while (labelStart < colon && iswspace(text[labelStart])) ++labelStart;
    size_t labelEnd = colon;
    while (labelEnd > labelStart && iswspace(text[labelEnd - 1])) --labelEnd;
    if (labelStart == labelEnd) return false;

    for (size_t i = labelStart; i < labelEnd; ++i) {
        const wchar_t ch = text[i];
        if (iswalnum(ch) || iswspace(ch) || ch > 127 || ch == L'&' || ch == L'/' || ch == L'\\' ||
            ch == L'.' || ch == L'+' || ch == L'-') continue;
        return false;
    }
    return true;
}

std::wstring normalized_header_identity(const std::wstring& rawText) {
    std::wstring text;
    text.reserve(rawText.size());
    for (wchar_t ch : rawText) {
        if (iswalnum(ch) || ch > 127) text.push_back(static_cast<wchar_t>(towlower(ch)));
    }
    return text;
}

bool same_header_identity(const std::wstring& first, const std::wstring& second) {
    const std::wstring normalizedFirst = normalized_header_identity(first);
    const std::wstring normalizedSecond = normalized_header_identity(second);
    return !normalizedFirst.empty() && normalizedFirst == normalizedSecond;
}

bool split_spaced_title_artist_header(const std::wstring& rawText, std::wstring& left, std::wstring& right) {
    const std::wstring text = normalized_credit_line(rawText);
    static const wchar_t separators[] = { L'-', L'\u2013', L'\u2014', L'\uff0d' };
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        if (std::find(std::begin(separators), std::end(separators), text[i]) == std::end(separators)) continue;
        if (!iswspace(text[i - 1]) || !iswspace(text[i + 1])) continue;

        left = normalized_credit_line(text.substr(0, i));
        right = normalized_credit_line(text.substr(i + 1));
        if (!left.empty() && !right.empty()) return true;
    }
    return false;
}

bool looks_like_leading_title_artist_header(
    const std::vector<lyric_jump_item>& items,
    size_t index,
    const current_track_search_info& trackInfo) {
    if (index >= items.size() || index >= 5) return false;

    std::wstring left;
    std::wstring right;
    if (!split_spaced_title_artist_header(items[index].text, left, right)) return false;

    const bool titleMatches = same_header_identity(left, trackInfo.title) || same_header_identity(right, trackInfo.title);
    if (!titleMatches) return false;

    const bool artistMatches = same_header_identity(left, trackInfo.artist) || same_header_identity(right, trackInfo.artist);
    if (!trackInfo.artist.empty() && artistMatches) return true;

    // When the local file has no artist tag, a long instrumental intro after a
    // zero-time "title - artist" line is strong evidence that the line is a
    // metadata header, not an ordinary lyric containing a dash.
    const bool startsAtBeginning = items[index].time_ms <= 3000;
    const bool longGapAfterHeader = index + 1 < items.size() &&
        items[index + 1].time_ms - items[index].time_ms >= 8000;
    if (trackInfo.artist.empty() && startsAtBeginning && longGapAfterHeader) return true;

    // Credit fields immediately following the line provide the same evidence
    // for songs whose vocal starts sooner.
    const size_t lastLookAhead = (std::min)(items.size(), index + 4);
    for (size_t next = index + 1; next < lastLookAhead; ++next) {
        if (is_leading_credit_line(items[next].text) || looks_like_generic_credit_field_line(items[next].text)) return true;
    }
    return false;
}

bool contains_any_marker(const std::wstring& text, const wchar_t* const* markers, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (text.find(markers[i]) != std::wstring::npos) return true;
    }
    return false;
}

bool looks_like_leading_banner_line(const std::wstring& rawText) {
    const std::wstring text = normalized_credit_line(rawText);
    if (text.empty() || text.size() > 160) return false;
    if (text.find(L"://") != std::wstring::npos || text.find(L"www.") != std::wstring::npos) return true;

    static const wchar_t* platformMarkers[] = {
        L"\u9177\u72d7", L"qq\u97f3\u4e50", L"\u7f51\u6613\u4e91", L"\u817e\u8baf\u97f3\u4e50", L"\u62d6\u97f3", L"\u5feb\u624b",
        L"bilibili", L"\u54aa\u5495\u97f3\u4e50", L"\u5343\u5343\u97f3\u4e50", L"\u97f3\u4e50\u4eba\u5e73\u53f0",
    };
    static const wchar_t* campaignMarkers[] = {
        L"\u8ba1\u5212", L"\u4f01\u5212", L"\u9879\u76ee", L"\u72ec\u5bb6\u5448\u73b0", L"\u8054\u5408\u5448\u73b0", L"\u8363\u8a89\u5448\u73b0",
        L"\u5b98\u65b9\u9996\u53d1", L"\u7279\u522b\u9e23\u8c22", L"\u63a8\u5e7f", L"music project",
    };
    static const wchar_t* organizationMarkers[] = {
        L"\u6709\u9650\u516c\u53f8", L"\u6587\u5316\u4f20\u5a92", L"\u97f3\u4e50\u5382\u724c", L"\u97f3\u4e50\u5de5\u4f5c\u5ba4", L"studio presents",
    };

    const bool hasPlatform = contains_any_marker(text, platformMarkers, _countof(platformMarkers));
    const bool hasCampaign = contains_any_marker(text, campaignMarkers, _countof(campaignMarkers));
    const bool hasOrganization = contains_any_marker(text, organizationMarkers, _countof(organizationMarkers));
    if (hasOrganization) return true;
    if (hasPlatform && text.size() <= 80) return true;
    if (hasCampaign && text.size() <= 60) return true;

    const std::wstring trimmed = normalized_credit_line(rawText);
    if (trimmed.size() >= 2) {
        const wchar_t first = trimmed.front();
        const wchar_t last = trimmed.back();
        const bool fullyWrapped =
            (first == L'\u300c' && last == L'\u300d') || (first == L'\u300e' && last == L'\u300f') ||
            (first == L'\u3010' && last == L'\u3011') || (first == L'\u300a' && last == L'\u300b');
        if (fullyWrapped && (hasPlatform || hasCampaign || hasOrganization)) return true;
    }
    return false;
}

bool has_probable_lyric_language(const std::wstring& rawText) {
    const std::wstring text = normalized_credit_line(rawText);
    if (text.empty() || text.size() > 180 || text.find(L"://") != std::wstring::npos) return false;
    if (is_leading_credit_line(text) || looks_like_generic_credit_field_line(text) ||
        looks_like_unlabeled_artist_list(text) || looks_like_leading_banner_line(text)) return false;

    size_t latinWords = 0;
    bool insideLatinWord = false;
    for (wchar_t ch : text) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z')) {
            if (!insideLatinWord) ++latinWords;
            insideLatinWord = true;
        } else {
            insideLatinWord = false;
        }
        if ((ch >= 0x3040 && ch <= 0x30ff) || (ch >= 0xac00 && ch <= 0xd7af)) return true;
    }
    if (latinWords >= 2) return true;

    // Common grammatical and imagery characters are deliberately broad. They
    // are only used after a confirmed leading metadata block and together with
    // neighbouring lines, not as a standalone deletion rule.
    static const wchar_t* lyricCues =
        L"\u6211\u4f60\u4ed6\u5979\u5b83\u8c01\u7231\u5fc3\u60c5\u68a6\u6cea\u98ce\u96e8\u6708\u591c\u5929\u6d77\u8def\u4eba"
        L"\u4e0d\u6ca1\u5728\u662f\u6709\u8ba9\u628a\u60f3\u770b\u542c\u8bf4\u8d70\u6765\u53bb\u7b49\u7740\u4e86\u7684\u4e5f\u53c8"
        L"\u4f1a\u80fd\u8981\u53ea\u90fd\u8fd8\u5374\u5982\u4e0e\u548c\u5411\u4ece\u4e3a\u5c06\u522b\u88ab\u7ed9\u8fc7\u8fd9\u90a3\u4f55";
    return text.find_first_of(lyricCues) != std::wstring::npos;
}

bool line_repeats_later(const std::vector<lyric_jump_item>& items, size_t index) {
    if (index >= items.size()) return false;
    const std::wstring identity = normalized_header_identity(items[index].text);
    if (identity.size() < 2) return false;
    for (size_t i = index + 2; i < items.size(); ++i) {
        if (same_header_identity(identity, items[i].text)) return true;
    }
    return false;
}

bool looks_like_probable_body_start(const std::vector<lyric_jump_item>& items, size_t index) {
    if (index >= items.size()) return false;
    const std::wstring current = normalized_credit_line(items[index].text);
    if (current.empty() || looks_like_leading_banner_line(current) || is_leading_credit_line(current) ||
        looks_like_generic_credit_field_line(current)) return false;

    if (line_repeats_later(items, index)) return true;

    size_t usableLines = 0;
    size_t lyricLikeLines = 0;
    constexpr size_t kSequenceLength = 3;
    const size_t end = (std::min)(items.size(), index + 4);
    for (size_t i = index; i < end && usableLines < kSequenceLength; ++i) {
        if (items[i].text.empty()) continue;
        if (looks_like_leading_banner_line(items[i].text) || is_leading_credit_line(items[i].text) ||
            looks_like_generic_credit_field_line(items[i].text)) break;
        ++usableLines;
        if (has_probable_lyric_language(items[i].text)) ++lyricLikeLines;
    }
    if (usableLines < kSequenceLength || lyricLikeLines < 2) return false;

    // A short opening call such as "MAMA" may not itself contain a language
    // cue, but is retained when followed by a convincing lyric sequence.
    return has_probable_lyric_language(current) || normalized_header_identity(current).size() <= 20;
}

size_t detect_leading_body_start(const std::vector<lyric_jump_item>& items, size_t fallbackStart) {
    if (fallbackStart >= items.size()) return fallbackStart;
    const size_t end = (std::min)(items.size(), fallbackStart + 10);
    for (size_t i = fallbackStart; i < end; ++i) {
        if (items[i].text.empty() || looks_like_leading_banner_line(items[i].text) ||
            is_leading_credit_line(items[i].text) || looks_like_generic_credit_field_line(items[i].text)) continue;
        if (looks_like_probable_body_start(items, i)) return i;
    }
    return fallbackStart;
}

std::vector<lyric_jump_item> filter_leading_credits_impl(const std::vector<lyric_jump_item>& items) {
    if (items.empty()) return items;

    constexpr size_t kMaximumLinesToInspect = 40;
    constexpr size_t kOrdinaryLinesBeforeBody = 3;
    size_t lastCredit = static_cast<size_t>(-1);
    size_t ordinaryLines = 0;
    const current_track_search_info trackInfo = get_current_track_search_info();

    for (size_t i = 0; i < items.size(); ++i) {
        // Only discover a credit block near the beginning. Once found, keep
        // scanning until several ordinary lyric lines prove that the body began.
        if (lastCredit == static_cast<size_t>(-1) && i >= kMaximumLinesToInspect) break;

        bool isCredit = looks_like_leading_title_artist_header(items, i, trackInfo) ||
            is_leading_credit_line(items[i].text) ||
            (i < 10 && looks_like_unlabeled_artist_list(items[i].text));
        if (!isCredit && looks_like_generic_credit_field_line(items[i].text)) {
            const bool continuesCreditBlock = lastCredit != static_cast<size_t>(-1) && lastCredit + 1 == i;
            const bool nextIsGenericCredit = i + 1 < items.size() && i + 1 < 20 &&
                looks_like_generic_credit_field_line(items[i + 1].text);
            isCredit = i < 5 || continuesCreditBlock || (i < 20 && nextIsGenericCredit);
        }
        if (isCredit) {
            lastCredit = i;
            ordinaryLines = 0;
        } else if (lastCredit != static_cast<size_t>(-1) && ++ordinaryLines >= kOrdinaryLinesBeforeBody) {
            break;
        }
    }

    if (lastCredit == static_cast<size_t>(-1)) return items;
    const size_t bodyStart = detect_leading_body_start(items, lastCredit + 1);
    return std::vector<lyric_jump_item>(items.begin() + bodyStart, items.end());
}

std::wstring build_plain_text(const std::vector<lyric_jump_item>& items) {
    std::wstring text;
    for (const auto& item : items) {
        if (item.text.empty()) continue;
        text += item.text;
        text += L"\r\n";
    }
    return text;
}

std::wstring build_timestamp_text(const std::vector<lyric_jump_item>& items) {
    std::wstring text;
    for (const auto& item : items) {
        if (item.text.empty()) continue;
        text += format_timestamp(item.time_ms);
        text += item.text;
        text += L"\r\n";
    }
    return text;
}

std::wstring build_split_text(const std::vector<lyric_jump_item>& items) {
    std::wstring plain = build_plain_text(items);
    pfc::string8 configured = cfg_copy_split_separators.get();
    std::wstring separators = pfc::stringcvt::string_wide_from_utf8(configured.get_ptr()).get_ptr();
    if (separators.empty()) return plain;

    std::wstring text;
    text.reserve(plain.size() + 64);
    for (size_t i = 0; i < plain.size(); ++i) {
        wchar_t ch = plain[i];
        text.push_back(ch);
        if (contains_split_char(separators, ch)) {
            size_t next = i + 1;
            if (next < plain.size() && !is_line_break(plain[next])) text += L"\r\n";
        }
    }
    return text;
}

bool copy_text_with_message(const std::wstring& text) {
    if (text.empty()) {
        speech_queue_speak(L"\u5f53\u524dLRC\u6ca1\u6709\u53ef\u590d\u5236\u7684\u6b4c\u8bcd\u6587\u672c", true);
        return false;
    }

    bool ok = copy_text_to_clipboard(text);
    speech_queue_speak(ok ? L"\u6b4c\u8bcd\u590d\u5236\u6210\u529f" : L"\u6b4c\u8bcd\u590d\u5236\u5931\u8d25", true);
    return ok;
}

bool run_copy_mode(lyric_copy_mode mode) {
    std::vector<lyric_jump_item> items = current_lyrics();
    if (cfg_copy_filter_leading_credits.get()) items = filter_leading_credits_impl(items);
    if (items.empty()) {
        speech_queue_speak(L"\u5f53\u524d\u6ca1\u6709\u5df2\u52a0\u8f7d\u7684LRC\u6b4c\u8bcd", true);
        return false;
    }

    switch (mode) {
    case lyric_copy_mode::timestamps:
        return copy_text_with_message(build_timestamp_text(items));
    case lyric_copy_mode::plain:
        return copy_text_with_message(build_plain_text(items));
    case lyric_copy_mode::split:
        return copy_text_with_message(build_split_text(items));
    case lyric_copy_mode::ask:
    default:
        return false;
    }
}

void choose_current_dialog_item(HWND wnd) {
    HWND list = GetDlgItem(wnd, IDC_COPY_MODE_LIST);
    int index = list ? static_cast<int>(SendMessageW(list, LB_GETCURSEL, 0, 0)) : LB_ERR;
    if (index < 0 || index >= static_cast<int>(_countof(k_dialog_modes))) index = 0;
    g_selected_dialog_mode = k_dialog_modes[index];
    EndDialog(wnd, IDOK);
}

INT_PTR CALLBACK copy_dialog_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_copy_dialog_wnd = wnd;
        SetWindowTextW(GetDlgItem(wnd, IDC_COPY_MODE_LIST), L"\u590d\u5236\u65b9\u5f0f");
        HWND list = GetDlgItem(wnd, IDC_COPY_MODE_LIST);
        if (list) {
            for (lyric_copy_mode mode : k_dialog_modes) {
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lyric_copy_mode_display_name(mode)));
            }
            SendMessageW(list, LB_SETCURSEL, 0, 0);
            SetFocus(list);
            return FALSE;
        }
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_COPY_MODE_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) {
                choose_current_dialog_item(wnd);
                return TRUE;
            }
            break;
        case IDOK:
            choose_current_dialog_item(wnd);
            return TRUE;
        case IDCANCEL:
            EndDialog(wnd, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NCDESTROY:
        if (g_copy_dialog_wnd == wnd) g_copy_dialog_wnd = nullptr;
        break;
    }
    return FALSE;
}

}

std::vector<lyric_jump_item> filter_leading_lyric_credits(const std::vector<lyric_jump_item>& items) {
    return filter_leading_credits_impl(items);
}

const char* lyric_copy_mode_to_id(lyric_copy_mode mode) {
    switch (mode) {
    case lyric_copy_mode::timestamps: return "timestamps";
    case lyric_copy_mode::plain: return "plain";
    case lyric_copy_mode::split: return "split";
    case lyric_copy_mode::ask:
    default:
        return "ask";
    }
}

lyric_copy_mode lyric_copy_mode_from_id(const char* id) {
    if (!id || !*id) return lyric_copy_mode::ask;
    if (_stricmp(id, "timestamps") == 0) return lyric_copy_mode::timestamps;
    if (_stricmp(id, "plain") == 0) return lyric_copy_mode::plain;
    if (_stricmp(id, "split") == 0) return lyric_copy_mode::split;
    return lyric_copy_mode::ask;
}

const wchar_t* lyric_copy_mode_display_name(lyric_copy_mode mode) {
    switch (mode) {
    case lyric_copy_mode::timestamps: return L"\u590d\u5236\u65f6\u95f4\u6233\u6b4c\u8bcd";
    case lyric_copy_mode::plain: return L"\u590d\u5236\u65e0\u65f6\u95f4\u6233\u6b4c\u8bcd";
    case lyric_copy_mode::split: return L"\u590d\u5236\u5206\u884c\u6b4c\u8bcd";
    case lyric_copy_mode::ask:
    default:
        return L"\u6309\u7528\u6237\u9009\u62e9\u590d\u5236";
    }
}

bool copy_current_lyrics(lyric_copy_mode mode) {
    if (mode == lyric_copy_mode::ask) return show_copy_lyrics_menu(core_api::get_main_window());
    return run_copy_mode(mode);
}

bool show_copy_lyrics_menu(HWND parent) {
    if (g_copy_dialog_wnd && IsWindow(g_copy_dialog_wnd)) {
        activate_copy_dialog();
        return false;
    }

    g_selected_dialog_mode = lyric_copy_mode::ask;
    HWND owner = parent && IsWindow(parent) && IsWindowVisible(parent) ? parent : nullptr;
    INT_PTR result = DialogBoxParamW(core_api::get_my_instance(), MAKEINTRESOURCEW(IDD_COPY_LYRICS), owner, copy_dialog_proc, 0);
    if (result != IDOK || g_selected_dialog_mode == lyric_copy_mode::ask) return false;
    return run_copy_mode(g_selected_dialog_mode);
}
