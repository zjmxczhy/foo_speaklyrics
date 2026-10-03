#include "stdafx.h"
#include "speaklyrics_log.h"
#include "config.h"
#include "log_file_sink.h"
#include "log_privacy.h"

namespace {

SRWLOCK g_log_lock = SRWLOCK_INIT;
speaklyrics_log_io::sink g_log_sink;
std::atomic<bool> g_detailed_logging{false};
std::wstring g_log_identity;

class log_lock_guard {
public:
    log_lock_guard() { AcquireSRWLockExclusive(&g_log_lock); }
    ~log_lock_guard() { ReleaseSRWLockExclusive(&g_log_lock); }
    log_lock_guard(const log_lock_guard&) = delete;
    log_lock_guard& operator=(const log_lock_guard&) = delete;
};

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

std::wstring fb2k_path_to_native_wide(const char* path) {
    if (!path || !*path) return L"";

    pfc::string8 native;
    if (foobar2000_io::extract_native_path(path, native)) return utf8_to_wide(native.get_ptr());

    return utf8_to_wide(path);
}

std::string current_component_version() {
    const char* currentFileName = core_api::get_my_file_name();
    if (!currentFileName || !*currentFileName) return std::string();

    componentversion::ptr component;
    service_enum_t<componentversion> enumeration;
    pfc::string8 fileName;
    pfc::string8 version;
    while (enumeration.next(component)) {
        fileName = "";
        component->get_file_name(fileName);
        if (_stricmp(fileName.get_ptr(), currentFileName) != 0) continue;

        version = "";
        component->get_component_version(version);
        return version.get_ptr();
    }

    return std::string();
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

const wchar_t* io_operation_name(speaklyrics_log_io::operation action) {
    using speaklyrics_log_io::operation;
    switch (action) {
    case operation::inspect: return L"检查文件大小";
    case operation::rotate: return L"轮转并替换旧日志";
    case operation::open: return L"打开日志文件";
    case operation::write: return L"写入日志文件";
    case operation::close: return L"关闭日志文件";
    default: return L"未知操作";
    }
}

std::wstring io_notice_line(const speaklyrics_log_io::notice& notice,
    const std::wstring& path) {
    using speaklyrics_log_io::notice_kind;
    std::wstring message = L"日志诊断：";
    const wchar_t* level = L"INFO";
    if (notice.kind == notice_kind::failure) {
        level = L"WARN";
        message += io_operation_name(notice.action);
        message += L"失败，系统错误码=" + std::to_wstring(notice.error);
        message += L"，累计失败=" + std::to_wstring(notice.occurrences);
        if (notice.action == speaklyrics_log_io::operation::write) {
            message += L"，预计字节=" + std::to_wstring(notice.expected_bytes);
            message += L"，已写字节=" + std::to_wstring(notice.actual_bytes);
        }
        if (notice.action == speaklyrics_log_io::operation::rotate) {
            message += L"，当前日志字节=" + std::to_wstring(notice.file_bytes);
            message += L"；原日志和旧备份均保留，稍后重试轮转";
        }
        message += L"。相同错误每30秒最多提示一次。";
    } else if (notice.kind == notice_kind::recovered) {
        message += io_operation_name(notice.action);
        message += L"已恢复，上次系统错误码=" + std::to_wstring(notice.error);
        message += L"，此前累计失败=" + std::to_wstring(notice.occurrences) + L"。";
    } else if (notice.kind == notice_kind::rotated) {
        message += L"已轮转旧日志，旧日志字节=" + std::to_wstring(notice.file_bytes);
        message += L"，备份=" + speaklyrics_log_path((path + L".old").c_str()) + L"。";
        message += g_log_identity;
    } else {
        message += L"日志写入已恢复，此前未完整写入=" +
            std::to_wstring(notice.occurrences) + L"条；这些记录无法补回。";
    }
    message += L" 文件=" + speaklyrics_log_path(path.c_str());
    return L"[" + current_time_text() + L"] [" + level + L"] " +
        speaklyrics_log_privacy::message(message, g_detailed_logging.load());
}

std::vector<std::wstring> write_file_line(const std::wstring& line) {
    const std::wstring path = speaklyrics_log_file_path();
    if (path.empty()) return {};
    const std::string bytes = wide_to_utf8(line) + "\r\n";
    std::vector<std::wstring> diagnostics;
    log_lock_guard lock;
    const uint64_t now = GetTickCount64();
    auto result = g_log_sink.append(path, bytes, now);
    std::string diagnosticBytes;
    for (const auto& notice : result.notices) {
        diagnostics.push_back(io_notice_line(notice, path));
        diagnosticBytes += wide_to_utf8(diagnostics.back()) + "\r\n";
    }
    // Save transitions when the file is writable. A diagnostic about a failed
    // diagnostic write goes only to the console, never back into this function.
    if (result.success && !diagnosticBytes.empty()) {
        const auto diagnosticResult = g_log_sink.append(path, diagnosticBytes, now);
        for (const auto& notice : diagnosticResult.notices)
            diagnostics.push_back(io_notice_line(notice, path));
    }
    return diagnostics; // RAII releases the lock before any console callback.
}

void console_print_safe(const char* text) {
    __try {
        if (core_api::are_services_available()) console::print(text);
        else OutputDebugStringA(text);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void write_console_line(const std::wstring& line) {
    std::string utf8 = wide_to_utf8(line);
    console_print_safe(utf8.c_str());
}

void log_v(const wchar_t* level, const wchar_t* format, va_list args) noexcept {
    try {
        const std::wstring message = speaklyrics_log_privacy::message(
            format_message(format, args), g_detailed_logging.load());
        const std::wstring line = L"[" + current_time_text() + L"] [" + level + L"] " + message;
        const auto diagnostics = write_file_line(line);
        for (const auto& diagnostic : diagnostics) write_console_line(diagnostic);
        write_console_line(line);
    } catch (...) {
        // Logging must never throw back into a playback or speech callback.
        console_print_safe("foo_speaklyrics: could not prepare log record; file logging failed.");
    }
}

}

std::wstring speaklyrics_log_file_path() {
    if (!core_api::are_services_available()) return L"";
    pfc::string8 path = core_api::pathInProfile("foo_speaklyrics.log");
    return fb2k_path_to_native_wide(path.get_ptr());
}

void speaklyrics_log_startup() {
    static std::atomic_flag startupLogged = ATOMIC_FLAG_INIT;
    if (speaklyrics_log_file_path().empty()) return;
    if (startupLogged.test_and_set()) return;

    g_detailed_logging.store(cfg_detailed_diagnostic_log.get());
    const std::string version = current_component_version();
    const std::string architecture = pfc::cpuArch();
    const std::wstring versionWide = utf8_to_wide(
        version.empty() ? "unknown" : version.c_str());
    const std::wstring architectureWide = utf8_to_wide(
        architecture.empty() ? "unknown" : architecture.c_str());

    {
        log_lock_guard lock;
        g_log_identity = L"组件版本=" + versionWide + L"，架构=" + architectureWide +
            L"，进程ID=" + std::to_wstring(GetCurrentProcessId()) + L"。";
    }
    speaklyrics_log_info(
        L"朗读LRC歌词组件启动：版本=%s，架构=%s，进程ID=%lu。",
        versionWide.c_str(), architectureWide.c_str(),
        static_cast<unsigned long>(GetCurrentProcessId()));
    speaklyrics_log_info(L"日志模式：%s；单个日志约8MiB，旧日志仅保留一份并在下次轮转时替换。",
        g_detailed_logging.load() ? L"详细诊断（包含完整路径和文本片段）" : L"普通诊断（隐藏目录结构和文本片段）");
}

std::wstring speaklyrics_log_text_excerpt(const wchar_t* text, size_t maximumCharacters) {
    if (!text || maximumCharacters == 0) return L"";
    if (!g_detailed_logging.load()) return speaklyrics_log_privacy::private_text(text, false);

    std::wstring excerpt;
    excerpt.reserve(maximumCharacters + 3);
    bool previousWasSpace = false;
    for (const wchar_t* cursor = text; *cursor && excerpt.size() < maximumCharacters; ++cursor) {
        wchar_t ch = *cursor;
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        if (iswspace(ch)) {
            if (previousWasSpace) continue;
            ch = L' ';
            previousWasSpace = true;
        } else {
            previousWasSpace = false;
        }
        excerpt.push_back(ch);
    }

    while (!excerpt.empty() && excerpt.back() == L' ') excerpt.pop_back();
    if (wcslen(text) > maximumCharacters) excerpt += L"...";
    return excerpt;
}

uint64_t speaklyrics_log_text_hash(const wchar_t* text) {
    return speaklyrics_log_privacy::text_hash(text);
}

std::wstring speaklyrics_log_path(const wchar_t* path) {
    return speaklyrics_log_privacy::path_label(path ? path : L"", g_detailed_logging.load());
}

std::wstring speaklyrics_log_private_text(const wchar_t* text) {
    return speaklyrics_log_privacy::private_text(text ? text : L"", g_detailed_logging.load());
}

void speaklyrics_log_set_detailed(bool enabled) {
    if (g_detailed_logging.exchange(enabled) == enabled) return;
    speaklyrics_log_info(L"日志模式已切换：%s；仅影响后续记录。",
        enabled ? L"详细诊断（包含完整路径和文本片段）" : L"普通诊断（隐藏目录结构和文本片段）");
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
