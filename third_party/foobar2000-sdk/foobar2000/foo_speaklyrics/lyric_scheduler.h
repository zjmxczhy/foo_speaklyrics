#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

// Timestamp and identity rules are kept independent from foobar2000 and the
// speech backends so the scheduler can be tested as a pure component.
struct lyric_schedule_item {
    uint64_t line_id = 0;
    size_t document_index = 0;
    int lyric_time_ms = 0;
    int trigger_time_ms = 0;
    size_t same_timestamp_order = 0;
    std::wstring text;
};

namespace lyric_scheduler {

inline bool same_event(const lyric_schedule_item& first, const lyric_schedule_item& second) {
    return first.lyric_time_ms == second.lyric_time_ms &&
        first.trigger_time_ms == second.trigger_time_ms;
}

inline std::vector<size_t> collect_due_indices(
    const std::vector<lyric_schedule_item>& items,
    int previous_trigger_ms,
    int current_trigger_ms,
    const std::unordered_set<uint64_t>& handled_line_ids,
    const std::unordered_set<uint64_t>& retry_line_ids) {
    std::vector<size_t> result;
    if (current_trigger_ms < previous_trigger_ms) return result;

    for (size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        if (item.trigger_time_ms > current_trigger_ms) break;

        const bool in_interval = item.trigger_time_ms > previous_trigger_ms;
        const bool retry = item.line_id != 0 &&
            retry_line_ids.find(item.line_id) != retry_line_ids.end();
        if (!in_interval && !retry) continue;

        if (item.line_id != 0 && handled_line_ids.find(item.line_id) != handled_line_ids.end()) {
            continue;
        }
        result.push_back(index);
    }
    return result;
}

inline std::vector<size_t> find_latest_seek_group(
    const std::vector<lyric_schedule_item>& items, int target_trigger_ms) {
    size_t latest = static_cast<size_t>(-1);
    for (size_t index = 0; index < items.size(); ++index) {
        if (items[index].trigger_time_ms <= target_trigger_ms) latest = index;
        else break;
    }
    if (latest == static_cast<size_t>(-1)) return {};

    size_t first = latest;
    while (first > 0 && same_event(items[first - 1], items[latest])) --first;

    std::vector<size_t> result;
    for (size_t index = first; index <= latest; ++index) {
        result.push_back(index);
    }
    return result;
}

inline std::vector<std::vector<size_t>> group_indices_by_event(
    const std::vector<lyric_schedule_item>& items,
    const std::vector<size_t>& indices) {
    std::vector<std::vector<size_t>> groups;
    for (const size_t index : indices) {
        if (index >= items.size()) continue;
        if (groups.empty() || !same_event(items[groups.back().back()], items[index])) {
            groups.push_back({});
        }
        groups.back().push_back(index);
    }
    return groups;
}

} // namespace lyric_scheduler
