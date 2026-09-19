#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/lyric_scheduler.h"

#include <cassert>
#include <iostream>

namespace {

lyric_schedule_item make_item(uint64_t lineId, int lyricTime, int triggerTime,
    const wchar_t* text, size_t documentIndex = 0, size_t sameTimestampOrder = 0) {
    lyric_schedule_item item;
    item.line_id = lineId;
    item.document_index = documentIndex;
    item.lyric_time_ms = lyricTime;
    item.trigger_time_ms = triggerTime;
    item.same_timestamp_order = sameTimestampOrder;
    item.text = text;
    return item;
}

void test_duplicate_timestamp_keeps_all_lines_in_source_order() {
    const std::vector<lyric_schedule_item> items = {
        make_item(11, 1000, 1000, L"first", 0, 0),
        make_item(12, 1000, 1000, L"second", 1, 1),
        make_item(13, 2000, 2000, L"third", 2, 0),
    };

    const std::unordered_set<uint64_t> empty;
    const std::vector<size_t> due = lyric_scheduler::collect_due_indices(
        items, 0, 1000, empty, empty);
    assert((due == std::vector<size_t>{ 0, 1 }));

    const auto groups = lyric_scheduler::group_indices_by_event(items, due);
    assert(groups.size() == 1);
    assert((groups.front() == std::vector<size_t>{ 0, 1 }));
}

void test_identical_text_with_distinct_line_ids_is_not_collapsed() {
    const std::vector<lyric_schedule_item> items = {
        make_item(21, 1000, 1000, L"same", 0, 0),
        make_item(22, 1000, 1000, L"same", 1, 1),
    };

    const std::unordered_set<uint64_t> empty;
    const std::vector<size_t> due = lyric_scheduler::collect_due_indices(
        items, 0, 1000, empty, empty);
    assert((due == std::vector<size_t>{ 0, 1 }));
    assert(items[due[0]].line_id != items[due[1]].line_id);
}

void test_delayed_callback_collects_every_crossed_event() {
    const std::vector<lyric_schedule_item> items = {
        make_item(31, 1000, 1000, L"one"),
        make_item(32, 1400, 1400, L"two"),
        make_item(33, 1800, 1800, L"three"),
    };

    const std::unordered_set<uint64_t> empty;
    const std::vector<size_t> due = lyric_scheduler::collect_due_indices(
        items, 900, 1900, empty, empty);
    assert((due == std::vector<size_t>{ 0, 1, 2 }));
}

void test_handled_lines_are_excluded_and_retry_lines_return() {
    const std::vector<lyric_schedule_item> items = {
        make_item(41, 1000, 1000, L"handled"),
        make_item(42, 1500, 1500, L"retry"),
        make_item(43, 2200, 2200, L"future"),
    };
    const std::unordered_set<uint64_t> handled = { 41 };
    const std::unordered_set<uint64_t> retry = { 42 };

    const std::vector<size_t> due = lyric_scheduler::collect_due_indices(
        items, 2000, 2100, handled, retry);
    assert((due == std::vector<size_t>{ 1 }));
}

void test_seek_returns_only_latest_target_group() {
    const std::vector<lyric_schedule_item> items = {
        make_item(51, 1000, 1000, L"old"),
        make_item(52, 2000, 2000, L"target first", 1, 0),
        make_item(53, 2000, 2000, L"target second", 2, 1),
        make_item(54, 3000, 3000, L"future"),
    };

    const std::vector<size_t> group = lyric_scheduler::find_latest_seek_group(items, 2500);
    assert((group == std::vector<size_t>{ 1, 2 }));
}

void test_equal_trigger_times_do_not_merge_different_lyric_groups() {
    const std::vector<lyric_schedule_item> items = {
        make_item(55, 1000, 1000, L"first group"),
        make_item(56, 1500, 1000, L"second group"),
    };
    const std::vector<size_t> due = { 0, 1 };

    const auto groups = lyric_scheduler::group_indices_by_event(items, due);
    assert(groups.size() == 2);
    assert((groups[0] == std::vector<size_t>{ 0 }));
    assert((groups[1] == std::vector<size_t>{ 1 }));

    const std::vector<size_t> seekGroup =
        lyric_scheduler::find_latest_seek_group(items, 1000);
    assert((seekGroup == std::vector<size_t>{ 1 }));
}

void test_new_position_epoch_can_schedule_the_same_line_again() {
    const std::vector<lyric_schedule_item> items = {
        make_item(61, 1000, 1000, L"repeat after seek"),
    };
    const std::unordered_set<uint64_t> oldEpochHandled = { 61 };
    const std::unordered_set<uint64_t> empty;

    assert(lyric_scheduler::collect_due_indices(
        items, 0, 1000, oldEpochHandled, empty).empty());
    const std::vector<size_t> newEpochDue = lyric_scheduler::collect_due_indices(
        items, 0, 1000, empty, empty);
    assert((newEpochDue == std::vector<size_t>{ 0 }));
}

void test_resume_after_pause_replays_only_current_group() {
    const std::vector<lyric_schedule_item> items = {
        make_item(71, 9000, 9000, L"previous"),
        make_item(72, 12870, 12870, L"current first", 1, 0),
        make_item(73, 12870, 12870, L"current second", 2, 1),
        make_item(74, 17000, 17000, L"future"),
    };

    const std::vector<size_t> resumedGroup =
        lyric_scheduler::find_latest_seek_group(items, 14370);
    assert((resumedGroup == std::vector<size_t>{ 1, 2 }));
}

} // namespace

int main() {
    test_duplicate_timestamp_keeps_all_lines_in_source_order();
    test_identical_text_with_distinct_line_ids_is_not_collapsed();
    test_delayed_callback_collects_every_crossed_event();
    test_handled_lines_are_excluded_and_retry_lines_return();
    test_seek_returns_only_latest_target_group();
    test_equal_trigger_times_do_not_merge_different_lyric_groups();
    test_new_position_epoch_can_schedule_the_same_line_again();
    test_resume_after_pause_replays_only_current_group();
    std::cout << "lyric_scheduler tests passed\n";
    return 0;
}
