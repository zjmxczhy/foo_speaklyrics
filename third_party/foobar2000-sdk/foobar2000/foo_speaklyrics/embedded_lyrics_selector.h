#pragma once

#include <cstddef>
#include <optional>
#include <vector>

namespace speaklyrics_embedded_lyrics {

inline std::optional<std::size_t> select_best_value_index(
    const std::vector<std::size_t>& valid_line_counts) noexcept {
    std::optional<std::size_t> best_index;
    std::size_t best_line_count = 0;

    for (std::size_t index = 0; index < valid_line_counts.size(); ++index) {
        const std::size_t line_count = valid_line_counts[index];
        if (line_count == 0) continue;
        if (!best_index || line_count > best_line_count) {
            best_index = index;
            best_line_count = line_count;
        }
    }

    return best_index;
}

} // namespace speaklyrics_embedded_lyrics
