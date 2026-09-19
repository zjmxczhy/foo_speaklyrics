#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/tolk_call_policy.h"

#include <cassert>
#include <iostream>

namespace {

int g_call_count = 0;

bool return_true(const wchar_t*, bool) {
    ++g_call_count;
    return true;
}

bool return_false(const wchar_t*, bool) {
    ++g_call_count;
    return false;
}

void test_true_return_is_dispatched_once() {
    g_call_count = 0;
    const tolk_text_call_result result =
        invoke_tolk_text_once(&return_true, L"test", true);

    assert(g_call_count == 1);
    assert(result.status == tolk_text_call_status::returned_true);
    assert(tolk_text_call_was_dispatched(result));
    assert(!tolk_text_call_should_retry(result));
}

void test_false_return_is_dispatched_once_without_retry() {
    g_call_count = 0;
    const tolk_text_call_result result =
        invoke_tolk_text_once(&return_false, L"test", true);

    assert(g_call_count == 1);
    assert(result.status == tolk_text_call_status::returned_false);
    assert(tolk_text_call_was_dispatched(result));
    assert(!tolk_text_call_should_retry(result));
}

void test_missing_function_is_not_dispatched() {
    using text_function = bool(*)(const wchar_t*, bool);
    const text_function missing = nullptr;
    const tolk_text_call_result result =
        invoke_tolk_text_once(missing, L"test", false);

    assert(result.status == tolk_text_call_status::not_called);
    assert(!tolk_text_call_was_dispatched(result));
    assert(tolk_text_call_should_retry(result));
}

void test_exception_result_preserves_error_and_can_retry() {
    const tolk_text_call_result result = tolk_text_call_threw(0xC0000005u);

    assert(result.status == tolk_text_call_status::exception);
    assert(result.exception_code == 0xC0000005u);
    assert(!tolk_text_call_was_dispatched(result));
    assert(tolk_text_call_should_retry(result));
}

} // namespace

int main() {
    test_true_return_is_dispatched_once();
    test_false_return_is_dispatched_once_without_retry();
    test_missing_function_is_not_dispatched();
    test_exception_result_preserves_error_and_can_retry();
    std::cout << "tolk_call_policy tests passed" << std::endl;
    return 0;
}
