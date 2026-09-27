#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/embedded_lyrics_selector.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

void test_empty_and_unparseable_values_have_no_selection() {
    assert(!speaklyrics_embedded_lyrics::select_best_value_index({}));
    assert(!speaklyrics_embedded_lyrics::select_best_value_index({ 0, 0, 0 }));
}

void test_largest_parseable_value_is_selected() {
    const auto selected = speaklyrics_embedded_lyrics::select_best_value_index(
        { 0, 12, 48, 31 });
    assert(selected);
    assert(*selected == 2);
}

void test_equal_line_counts_keep_the_first_value() {
    const auto selected = speaklyrics_embedded_lyrics::select_best_value_index(
        { 24, 24, 18 });
    assert(selected);
    assert(*selected == 0);
}

void test_unparseable_values_do_not_affect_source_index() {
    const auto selected = speaklyrics_embedded_lyrics::select_best_value_index(
        { 0, 15, 0, 27, 0 });
    assert(selected);
    assert(*selected == 3);
}

} // namespace

int main() {
    test_empty_and_unparseable_values_have_no_selection();
    test_largest_parseable_value_is_selected();
    test_equal_line_counts_keep_the_first_value();
    test_unparseable_values_do_not_affect_source_index();
    std::cout << "embedded_lyrics_selector tests passed.\n";
    return 0;
}
