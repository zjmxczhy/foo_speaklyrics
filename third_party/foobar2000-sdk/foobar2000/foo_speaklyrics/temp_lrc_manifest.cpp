#include "stdafx.h"

#include "config.h"
#include "speaklyrics_log.h"
#include "temp_lrc_manifest.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kManifestMutexName[] =
    L"Local\\foo_speaklyrics.temp-lrc-manifest.v1";
constexpr unsigned long long kManifestMaxBytes = 1024ULL * 1024ULL;

std::wstring utf8_to_wide(const char* text) {
    return pfc::stringcvt::string_wide_from_utf8(text ? text : "").get_ptr();
}

std::wstring fb2k_path_to_native_wide(const char* path) {
    if (!path || !*path) return L"";

    pfc::string8 native;
    if (foobar2000_io::extract_native_path(path, native)) {
        return utf8_to_wide(native.get_ptr());
    }

    return utf8_to_wide(path);
}

std::string wide_to_utf8(const std::wstring& text) {
    return pfc::stringcvt::string_utf8_from_wide(text.c_str()).get_ptr();
}

std::wstring trim_line(std::wstring value) {
    while (!value.empty() &&
        (value.back() == L'\r' || value.back() == L'\n' ||
            value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
    }
    size_t start = 0;
    while (start < value.size() &&
        (value[start] == L'\r' || value[start] == L'\n' ||
            value[start] == L' ' || value[start] == L'\t')) {
        ++start;
    }
    if (start != 0) value.erase(0, start);
    if (!value.empty() && value.front() == 0xFEFF) value.erase(0, 1);
    return value;
}

bool same_path_ci(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

struct manifest_read_result {
    bool exists = false;
    bool readable = false;
    DWORD error = ERROR_SUCCESS;
    std::vector<std::wstring> paths;
};

manifest_read_result read_manifest_paths(const std::wstring& manifestPath) {
    manifest_read_result result;
    HANDLE file = CreateFileW(manifestPath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            result.readable = true;
            return result;
        }
        result.error = error;
        return result;
    }
    result.exists = true;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kManifestMaxBytes) {
        result.error = GetLastError();
        if (result.error == ERROR_SUCCESS) result.error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return result;
    }

    if (size.QuadPart == 0) {
        result.readable = true;
        CloseHandle(file);
        return result;
    }

    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const DWORD requested = static_cast<DWORD>(bytes.size());
    if (!ReadFile(file, bytes.data(), requested, &read, nullptr) || read != requested) {
        result.error = GetLastError();
        if (result.error == ERROR_SUCCESS) result.error = ERROR_READ_FAULT;
        CloseHandle(file);
        return result;
    }
    CloseHandle(file);

    bytes.resize(read);
    std::wstring text = utf8_to_wide(bytes.c_str());
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find_first_of(L"\r\n", start);
        const std::wstring path = trim_line(text.substr(start,
            end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!path.empty()) {
            const bool duplicate = std::any_of(result.paths.begin(), result.paths.end(),
                [&path](const std::wstring& existing) {
                    return same_path_ci(existing, path);
                });
            if (!duplicate) result.paths.push_back(path);
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }

    result.readable = true;
    return result;
}

bool write_manifest_atomically(const std::wstring& manifestPath,
    const std::vector<std::wstring>& paths) {
    std::wstring text;
    for (const auto& path : paths) {
        text += path;
        text += L"\r\n";
    }
    const std::string bytes = wide_to_utf8(text);

    const fs::path target(manifestPath);
    const std::wstring temporaryPath = manifestPath + L"." +
        std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L"." +
        std::to_wstring(static_cast<unsigned long long>(GetTickCount64())) +
        L".tmp";
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool ok = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, static_cast<size_t>(0x40000000)));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            ok = false;
            break;
        }
        offset += written;
    }
    if (ok && !FlushFileBuffers(file)) ok = false;
    CloseHandle(file);

    if (ok && !MoveFileExW(temporaryPath.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ok = false;
    }
    if (!ok) DeleteFileW(temporaryPath.c_str());
    return ok;
}

class manifest_mutex_guard {
public:
    manifest_mutex_guard() {
        m_handle = CreateMutexW(nullptr, FALSE, kManifestMutexName);
        if (!m_handle) return;

        const DWORD wait = WaitForSingleObject(m_handle, 5000);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
            m_owned = true;
            if (wait == WAIT_ABANDONED) {
                speaklyrics_log_warning(
                    L"Temporary lyric manifest mutex was abandoned; recovering the manifest.");
            }
        }
    }

    ~manifest_mutex_guard() {
        if (m_owned) ReleaseMutex(m_handle);
        if (m_handle) CloseHandle(m_handle);
    }

    bool acquired() const { return m_owned; }

private:
    HANDLE m_handle = nullptr;
    bool m_owned = false;
};

enum class manifest_path_safety {
    safe,
    unsafe,
    unresolved,
};

manifest_path_safety classify_manifest_path(const std::wstring& path) {
    if (path.empty()) return manifest_path_safety::unsafe;

    pfc::string8 configuredFolder = cfg_temp_lrc_folder.get();
    const std::wstring tempFolder = expand_environment_path(
        utf8_to_wide(configuredFolder.get_ptr()));
    if (tempFolder.empty()) return manifest_path_safety::unresolved;

    const fs::path filePath(path);
    if (!filePath.is_absolute() ||
        _wcsicmp(filePath.extension().c_str(), L".lrc") != 0 ||
        !filePath.has_parent_path()) {
        return manifest_path_safety::unsafe;
    }

    std::error_code error;
    const fs::path canonicalFolder = fs::weakly_canonical(
        fs::path(tempFolder), error);
    if (error) return manifest_path_safety::unresolved;
    error.clear();
    const fs::path canonicalParent = fs::weakly_canonical(
        filePath.parent_path(), error);
    if (error) return manifest_path_safety::unresolved;

    return same_path_ci(canonicalParent.wstring(), canonicalFolder.wstring())
        ? manifest_path_safety::safe : manifest_path_safety::unsafe;
}

enum class manifest_file_state {
    present,
    missing,
    invalid,
    unresolved,
};

manifest_file_state get_manifest_file_state(const std::wstring& path,
    DWORD& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return manifest_file_state::missing;
        }
        return manifest_file_state::unresolved;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        error = ERROR_DIRECTORY;
        return manifest_file_state::invalid;
    }
    error = ERROR_SUCCESS;
    return manifest_file_state::present;
}

} // namespace

std::wstring temp_lrc_manifest_path() {
    if (!core_api::are_services_available()) return L"";
    pfc::string8 path = core_api::pathInProfile("foo_speaklyrics_temp_lrc_manifest.txt");
    return fb2k_path_to_native_wide(path.get_ptr());
}

void temp_lrc_manifest_cleanup() {
    try {
        const std::wstring manifestPath = temp_lrc_manifest_path();
        if (manifestPath.empty()) return;

        manifest_mutex_guard mutex;
        if (!mutex.acquired()) {
            speaklyrics_log_warning(
                L"Temporary lyric manifest cleanup skipped: could not acquire the cross-process mutex.");
            return;
        }

        const manifest_read_result read = read_manifest_paths(manifestPath);
        if (!read.readable) {
            speaklyrics_log_warning(
                L"Temporary lyric manifest cleanup skipped: read failed, error code=%lu.",
                static_cast<unsigned long>(read.error));
            return;
        }
        if (!read.exists) return;

        std::vector<std::wstring> retained;
        retained.reserve(read.paths.size());
        for (const auto& path : read.paths) {
            manifest_path_safety safety = manifest_path_safety::unresolved;
            try {
                safety = classify_manifest_path(path);
            }
            catch (...) {
                retained.push_back(path);
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup preserved a manifest entry that could not be classified: %s.",
                    speaklyrics_log_path(path.c_str()).c_str());
                continue;
            }
            if (safety == manifest_path_safety::unsafe) {
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup removed an unsafe manifest entry without deleting it: %s.",
                    speaklyrics_log_path(path.c_str()).c_str());
                continue;
            }
            if (safety == manifest_path_safety::unresolved) {
                retained.push_back(path);
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup preserved an unresolved manifest entry: %s.",
                    speaklyrics_log_path(path.c_str()).c_str());
                continue;
            }

            DWORD error = ERROR_SUCCESS;
            const manifest_file_state state = get_manifest_file_state(path, error);
            if (state == manifest_file_state::missing) {
                speaklyrics_log_info(
                    L"Temporary lyric cleanup removed an already missing entry: %s.",
                    speaklyrics_log_path(path.c_str()).c_str());
                continue;
            }
            if (state == manifest_file_state::invalid) {
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup removed a directory entry without deleting it: %s.",
                    speaklyrics_log_path(path.c_str()).c_str());
                continue;
            }
            if (state == manifest_file_state::unresolved) {
                retained.push_back(path);
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup preserved an inaccessible entry: %s, error code=%lu.",
                    speaklyrics_log_path(path.c_str()).c_str(), static_cast<unsigned long>(error));
                continue;
            }

            if (DeleteFileW(path.c_str())) {
                speaklyrics_log_info(L"Temporary lyric cleanup deleted: %s.", speaklyrics_log_path(path.c_str()).c_str());
            } else {
                error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                    continue;
                }
                retained.push_back(path);
                speaklyrics_log_warning(
                    L"Temporary lyric cleanup could not delete %s; retained for the next startup, error code=%lu.",
                    speaklyrics_log_path(path.c_str()).c_str(), static_cast<unsigned long>(error));
            }
        }

        if (!write_manifest_atomically(manifestPath, retained)) {
            speaklyrics_log_warning(
                L"Temporary lyric manifest cleanup could not atomically rewrite the manifest; original records were preserved.");
        }
    }
    catch (const std::exception& error) {
        speaklyrics_log_error(
            L"Temporary lyric manifest cleanup failed safely: %hs.", error.what());
    }
    catch (...) {
        speaklyrics_log_error(
            L"Temporary lyric manifest cleanup failed safely with an unknown exception.");
    }
}
