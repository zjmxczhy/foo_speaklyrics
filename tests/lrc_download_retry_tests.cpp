#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/lrc_download_retry.h"

#include <cassert>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

void test_transient_backoff_sequence_and_cap() {
    lrc_download_retry_state state;
    const std::wstring key = L"track-a";
    const std::vector<uint64_t> expected = {
        5000ULL, 15000ULL, 30000ULL, 60000ULL,
        120000ULL, 300000ULL, 300000ULL,
    };

    uint64_t now = 1000;
    for (size_t index = 0; index < expected.size(); ++index) {
        const uint32_t attempt = state.begin_attempt(key);
        assert(attempt == index + 1);
        const uint64_t delay = state.mark_transient_failure(
            key, now, L"network error");
        assert(delay == expected[index]);
        assert(!state.can_attempt(key, now + delay - 1));
        assert(state.can_attempt(key, now + delay));
        now += delay;
    }
}

void test_not_found_uses_long_cooldown() {
    lrc_download_retry_state state;
    const std::wstring key = L"track-b";
    state.begin_attempt(key);
    const uint64_t delay = state.mark_not_found(key, 5000, L"no result");
    assert(delay == 600000ULL);
    assert(!state.can_attempt(key, 604999));
    assert(state.can_attempt(key, 605000));
}

void test_configuration_blocks_until_reset_or_deadline() {
    lrc_download_retry_state state;
    const std::wstring key = L"track-c";
    state.mark_configuration_unavailable(key, 100, L"title missing");
    assert(!state.can_attempt(key, (std::numeric_limits<uint64_t>::max)() - 1));

    state.reset();
    assert(state.can_attempt(key, 100));

    state.mark_configuration_unavailable(key, 100, L"downloader missing",
        lrc_download_retry_policy::missing_downloader_delay_ms);
    assert(!state.can_attempt(key, 300099));
    assert(state.can_attempt(key, 300100));
}

void test_track_change_and_settings_reset_clear_old_state() {
    lrc_download_retry_state state;
    state.begin_attempt(L"track-d");
    state.mark_not_found(L"track-d", 0, L"no result");
    assert(state.can_attempt(L"track-e", 0));
    assert(state.begin_attempt(L"track-e") == 1);

    state.mark_transient_failure(L"track-e", 0, L"timeout");
    state.reset();
    assert(state.status == lrc_download_status::idle);
    assert(state.attempt_count == 0);
    assert(state.can_attempt(L"track-e", 0));
}

void test_success_and_in_progress_prevent_duplicate_requests() {
    lrc_download_retry_state state;
    const std::wstring key = L"track-f";
    assert(state.can_attempt(key, 0));
    assert(state.begin_attempt(key) == 1);
    assert(state.in_progress_for(key));
    assert(!state.can_attempt(key, 0));

    state.mark_succeeded(key);
    assert(!state.in_progress_for(key));
    assert(!state.can_attempt(key, (std::numeric_limits<uint64_t>::max)()));
    assert(state.attempt_count == 1);
}

void test_preflight_failure_does_not_increment_process_attempts() {
    lrc_download_retry_state state;
    const std::wstring key = L"track-g";
    const uint64_t delay = state.mark_transient_failure(
        key, 0, L"output folder unavailable");
    assert(delay == 5000ULL);
    assert(state.attempt_count == 0);
    assert(state.transient_failure_count == 1);
}

} // namespace

int main() {
    test_transient_backoff_sequence_and_cap();
    test_not_found_uses_long_cooldown();
    test_configuration_blocks_until_reset_or_deadline();
    test_track_change_and_settings_reset_clear_old_state();
    test_success_and_in_progress_prevent_duplicate_requests();
    test_preflight_failure_does_not_increment_process_attempts();
    std::cout << "lrc_download_retry tests passed" << std::endl;
    return 0;
}
