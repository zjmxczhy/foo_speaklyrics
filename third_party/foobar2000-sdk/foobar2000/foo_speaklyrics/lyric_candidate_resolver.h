#pragma once

#include <array>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>

namespace speaklyrics_lyric_resolver {

enum class candidate_source {
    manual_file,
    track_folder_exact,
    configured_folder_exact,
    embedded_lyrics,
    track_folder_fuzzy,
    configured_folder_fuzzy,
    temporary_folder,
};

enum class attempt_status {
    unavailable,
    rejected,
    accepted,
};

struct resolution_result {
    std::optional<candidate_source> selected_source;
    std::size_t considered_sources = 0;
    std::size_t rejected_candidates = 0;
};

inline constexpr std::array<candidate_source, 7> candidate_priority = {
    candidate_source::manual_file,
    candidate_source::track_folder_exact,
    candidate_source::configured_folder_exact,
    candidate_source::embedded_lyrics,
    candidate_source::track_folder_fuzzy,
    candidate_source::configured_folder_fuzzy,
    candidate_source::temporary_folder,
};

inline const wchar_t* source_name(candidate_source source) noexcept {
    switch (source) {
    case candidate_source::manual_file:
        return L"手动加载 LRC";
    case candidate_source::track_folder_exact:
        return L"歌曲同目录精确匹配";
    case candidate_source::configured_folder_exact:
        return L"正式 LRC 目录精确匹配";
    case candidate_source::embedded_lyrics:
        return L"音频文件内嵌 LYRICS 标签";
    case candidate_source::track_folder_fuzzy:
        return L"歌曲同目录模糊匹配";
    case candidate_source::configured_folder_fuzzy:
        return L"正式 LRC 目录模糊匹配";
    case candidate_source::temporary_folder:
        return L"临时 LRC 目录";
    }
    return L"未知歌词来源";
}

inline bool is_embedded(candidate_source source) noexcept {
    return source == candidate_source::embedded_lyrics;
}

inline bool is_temporary(candidate_source source) noexcept {
    return source == candidate_source::temporary_folder;
}

inline std::wstring path_key(const std::wstring& path) {
    std::wstring key = std::filesystem::path(path).lexically_normal().wstring();
    for (wchar_t& ch : key) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return key;
}

template <typename Range, typename Attempt>
attempt_status try_candidates_in_order(const Range& candidates, Attempt&& attempt) {
    bool rejected = false;
    for (const auto& candidate : candidates) {
        const attempt_status status = attempt(candidate);
        if (status == attempt_status::accepted) return status;
        if (status == attempt_status::rejected) rejected = true;
    }
    return rejected ? attempt_status::rejected : attempt_status::unavailable;
}

template <typename Attempt>
resolution_result resolve_in_priority_order(Attempt&& attempt) {
    resolution_result result;
    for (candidate_source source : candidate_priority) {
        ++result.considered_sources;
        const attempt_status status = attempt(source);
        if (status == attempt_status::rejected) {
            ++result.rejected_candidates;
            continue;
        }
        if (status == attempt_status::accepted) {
            result.selected_source = source;
            break;
        }
    }
    return result;
}

} // namespace speaklyrics_lyric_resolver
