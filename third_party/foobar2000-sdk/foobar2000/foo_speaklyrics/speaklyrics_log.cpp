#include "stdafx.h"
#include "speaklyrics_log.h"

namespace {

constexpr ULONGLONG kMaxLogBytes = 2 * 1024 * 1024;
SRWLOCK g_log_lock = SRWLOCK_INIT;

std::wstring format_message(const wchar_t* format, va_list args) {
    wchar_t buffer[4096] = {};
    va_list copy;
    va_copy(copy, args);
    int written = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format ? format : L"", copy);
    va_end(copy);
    if (written < 0 && buffer[0] == 0) return L"(log format failed)";
    return buffer;
}

std::wstring utf8_to_wide(const char* text) {
    if (!text) return L"";
    return pfc::stringcvt::string_wide_from_utf8(text).get_ptr();
}

std::string wide_to_utf8(const std::wstring& text) {
    return pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr();
}

std::wstring current_time_text() {
    SYSTEMTIME time = {};
    GetLocalTime(&time);

    wchar_t buffer[64] = {};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

void rotate_log_if_needed(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return;

    ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    if (size < kMaxLogBytes) return;

    std::wstring oldPath = path + L".old";
    DeleteFileW(oldPath.c_str());
    MoveFileExW(path.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void write_file_line(const std::wstring& line) {
    std::wstring path = speaklyrics_log_file_path();
    if (path.empty()) return;

    AcquireSRWLockExclusive(&g_log_lock);
    rotate_log_if_needed(path);

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ReleaseSRWLockExclusive(&g_log_lock);
        return;
    }

    std::string utf8 = wide_to_utf8(line);
    utf8 += "\r\n";

    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(file);
    ReleaseSRWLockExclusive(&g_log_lock);
}

void console_print_safe(const char* text) {
    __try {
        console::print(text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void write_console_line(const std::wstring& line) {
    std::string utf8 = wide_to_utf8(line);
    console_print_safe(utf8.c_str());
}

void log_v(const wchar_t* level, const wchar_t* format, va_list args) {
    std::wstring message = format_message(format, args);
    std::wstring line = L"[";
    line += current_time_text();
    line += L"] [";
    line += level;
    line += L"] ";
    line += message;

    write_file_line(line);
    write_console_line(line);
}

}

std::wstring speaklyrics_log_file_path() {
    if (!core_api::are_services_available()) return L"";
    pfc::string8 path = core_api::pathInProfile("foo_speaklyrics.log");
    return utf8_to_wide(path.get_ptr());
}

void speaklyrics_log_info(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    log_v(L"INFO", format, args);
    va_end(args);
}

void speaklyrics_log_warning(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    log_v(L"WARN", format, args);
    va_end(args);
}

void speaklyrics_log_error(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    log_v(L"ERROR", format, args);
    va_end(args);
}
