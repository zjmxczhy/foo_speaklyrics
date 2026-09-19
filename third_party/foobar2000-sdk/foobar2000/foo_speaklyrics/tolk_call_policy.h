#pragma once

#include <cstdint>

enum class tolk_text_call_status {
    not_called,
    returned_true,
    returned_false,
    exception,
};

struct tolk_text_call_result {
    tolk_text_call_status status = tolk_text_call_status::not_called;
    uint32_t exception_code = 0;
};

inline tolk_text_call_result tolk_text_call_returned(bool apiResult) {
    tolk_text_call_result result;
    result.status = apiResult ? tolk_text_call_status::returned_true :
        tolk_text_call_status::returned_false;
    return result;
}

inline tolk_text_call_result tolk_text_call_threw(uint32_t exceptionCode) {
    tolk_text_call_result result;
    result.status = tolk_text_call_status::exception;
    result.exception_code = exceptionCode;
    return result;
}

template<typename Fn>
tolk_text_call_result invoke_tolk_text_once(Fn fn, const wchar_t* text, bool interrupt) {
    if (!fn) return {};
    return tolk_text_call_returned(fn(text, interrupt));
}

inline bool tolk_text_call_was_dispatched(const tolk_text_call_result& result) {
    // Tolk documents the bool result as unreliable because of auto-detection.
    // A normally returned call is therefore considered submitted either way.
    return result.status == tolk_text_call_status::returned_true ||
        result.status == tolk_text_call_status::returned_false;
}

inline bool tolk_text_call_should_retry(const tolk_text_call_result& result) {
    return result.status == tolk_text_call_status::not_called ||
        result.status == tolk_text_call_status::exception;
}
