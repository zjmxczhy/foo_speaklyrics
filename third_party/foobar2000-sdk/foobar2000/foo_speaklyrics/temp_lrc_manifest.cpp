#include "stdafx.h"

#include "config.h"
#include "speaklyrics_log.h"
#include "temp_lrc_manifest.h"

namespace {

SRWLOCK g_manifest_lock = SRWLOCK_INIT;

std::wstring utf8_to_wide(const char* text) {
    return pfc::stringcvt::string_wide_from_utf8(text ? text : "").get_ptr();
}

std::wstring fb2k_path_to_native_wide(const char* path) {
    if (!path || !*path) return L"";

    pfc::string8 native;
    if (foobar2000_io::extract_native_path(path, native)) return utf8_to_wide(native.get_ptr());

    return utf8_to_wide(path);
}

std::string wide_to_utf8(const std::wstring& text) {
    return pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr();
}

std::wstring trim_line(std::wstring value) {
    while (!value.empty() && (value.back() == L'\r' || value.back() == L'\n' || value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() && (value[start] == L'\r' || value[start] == L'\n' || value[start] == L' ' || value[start] == L'\t')) {
        ++start;
    }
    return start == 0 ? value : value.substr(start);
}

std::vector<std::wstring> read_manifest_paths(const std::wstring& manifestPath) {
    std::vector<std::wstring> paths;

    HANDLE file = CreateFileW(manifestPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return paths;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return paths;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) {
        bytes.resize(read);
        std::wstring text = utf8_to_wide(bytes.c_str());
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find_first_of(L"\r\n", start);
            std::wstring line = trim_line(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
            if (!line.empty()) paths.push_back(line);
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }

    CloseHandle(file);
    return paths;
}

void clear_manifest_file(const std::wstring& manifestPath) {
    HANDLE file = CreateFileW(manifestPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

bool is_safe_manifest_lrc_path(const std::wstring& path) {
    if (path.empty()) return false;

    pfc::string8 configuredFolder = cfg_temp_lrc_folder.get();
    std::wstring tempFolder = expand_environment_path(utf8_to_wide(configuredFolder.get_ptr()));
    if (tempFolder.empty()) return false;

    std::filesystem::path filePath(path);
    if (_wcsicmp(filePath.extension().c_str(), L".lrc") != 0 || !filePath.has_parent_path()) return false;

    std::error_code error;
    const std::filesystem::path canonicalFolder = std::filesystem::weakly_canonical(std::filesystem::path(tempFolder), error);
    if (error) return false;
    const std::filesystem::path canonicalParent = std::filesystem::weakly_canonical(filePath.parent_path(), error);
    if (error) return false;

    return _wcsicmp(canonicalParent.c_str(), canonicalFolder.c_str()) == 0;
}

}

std::wstring temp_lrc_manifest_path() {
    if (!core_api::are_services_available()) return L"";
    pfc::string8 path = core_api::pathInProfile("foo_speaklyrics_temp_lrc_manifest.txt");
    return fb2k_path_to_native_wide(path.get_ptr());
}

void temp_lrc_manifest_cleanup() {
    std::wstring manifestPath = temp_lrc_manifest_path();
    if (manifestPath.empty()) return;

    AcquireSRWLockExclusive(&g_manifest_lock);
    std::vector<std::wstring> paths = read_manifest_paths(manifestPath);
    clear_manifest_file(manifestPath);
    ReleaseSRWLockExclusive(&g_manifest_lock);

    for (const auto& path : paths) {
        if (path.empty()) continue;
        if (!is_safe_manifest_lrc_path(path)) {
            speaklyrics_log_warning(L"Temporary lyric cleanup skipped an unsafe manifest path: %s.", path.c_str());
            continue;
        }
        DWORD attrs = GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (DeleteFileW(path.c_str())) {
            speaklyrics_log_info(L"临时歌词清理：已删除上次残留的临时 LRC：%s。", path.c_str());
        } else {
            speaklyrics_log_warning(L"临时歌词清理：删除失败：%s，错误码：%lu。", path.c_str(), GetLastError());
        }
    }
}
