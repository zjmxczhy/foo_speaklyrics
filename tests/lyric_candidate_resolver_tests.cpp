#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/lyric_candidate_resolver.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

using speaklyrics_lyric_resolver::attempt_status;
using speaklyrics_lyric_resolver::candidate_source;

void test_priority_order_is_stable() {
    const std::vector<candidate_source> expected(
        speaklyrics_lyric_resolver::candidate_priority.begin(),
        speaklyrics_lyric_resolver::candidate_priority.end());
    std::vector<candidate_source> visited;

    const auto result = speaklyrics_lyric_resolver::resolve_in_priority_order(
        [&](candidate_source source) {
            visited.push_back(source);
            return attempt_status::unavailable;
        });

    assert(!result.selected_source);
    assert(result.rejected_candidates == 0);
    assert(visited == expected);
}

void test_invalid_file_falls_back_to_embedded_lyrics() {
    std::vector<candidate_source> visited;
    const auto result = speaklyrics_lyric_resolver::resolve_in_priority_order(
        [&](candidate_source source) {
            visited.push_back(source);
            if (source == candidate_source::track_folder_exact) {
                return attempt_status::rejected;
            }
            if (source == candidate_source::embedded_lyrics) {
                return attempt_status::accepted;
            }
            return attempt_status::unavailable;
        });

    assert(result.selected_source);
    assert(*result.selected_source == candidate_source::embedded_lyrics);
    assert(result.rejected_candidates == 1);
    assert(visited.back() == candidate_source::embedded_lyrics);
}

void test_multiple_invalid_candidates_continue_to_temporary_folder() {
    const auto result = speaklyrics_lyric_resolver::resolve_in_priority_order(
        [](candidate_source source) {
            if (source == candidate_source::track_folder_exact ||
                source == candidate_source::configured_folder_exact ||
                source == candidate_source::configured_folder_fuzzy) {
                return attempt_status::rejected;
            }
            if (source == candidate_source::temporary_folder) {
                return attempt_status::accepted;
            }
            return attempt_status::unavailable;
        });

    assert(result.selected_source);
    assert(*result.selected_source == candidate_source::temporary_folder);
    assert(result.rejected_candidates == 3);
}

void test_invalid_candidate_falls_back_within_the_same_source() {
    const std::vector<int> candidates = { 1, 2, 3 };
    std::vector<int> visited;
    const auto status = speaklyrics_lyric_resolver::try_candidates_in_order(
        candidates,
        [&](int candidate) {
            visited.push_back(candidate);
            if (candidate == 1) return attempt_status::rejected;
            if (candidate == 2) return attempt_status::accepted;
            return attempt_status::rejected;
        });

    assert(status == attempt_status::accepted);
    assert((visited == std::vector<int>{ 1, 2 }));
}

void test_first_valid_candidate_stops_later_sources() {
    std::size_t attempts = 0;
    const auto result = speaklyrics_lyric_resolver::resolve_in_priority_order(
        [&](candidate_source source) {
            ++attempts;
            return source == candidate_source::manual_file
                ? attempt_status::accepted
                : attempt_status::rejected;
        });

    assert(result.selected_source);
    assert(*result.selected_source == candidate_source::manual_file);
    assert(attempts == 1);
    assert(result.rejected_candidates == 0);
}

void test_path_key_deduplicates_windows_path_spelling() {
    const std::wstring first = speaklyrics_lyric_resolver::path_key(
        L"C:/Lyrics/Album/../Song.LRC");
    const std::wstring second = speaklyrics_lyric_resolver::path_key(
        L"c:\\lyrics\\song.lrc");
    assert(first == second);
}

} // namespace

int main() {
    test_priority_order_is_stable();
    test_invalid_file_falls_back_to_embedded_lyrics();
    test_multiple_invalid_candidates_continue_to_temporary_folder();
    test_invalid_candidate_falls_back_within_the_same_source();
    test_first_valid_candidate_stops_later_sources();
    test_path_key_deduplicates_windows_path_spelling();
    std::cout << "lyric_candidate_resolver tests passed.\n";
    return 0;
}
