#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/log_file_sink.h"
#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/log_privacy.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

BOOL WINAPI fail_move(LPCWSTR, LPCWSTR, DWORD) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

BOOL WINAPI short_write(HANDLE file, LPCVOID buffer, DWORD bytes,
    LPDWORD written, LPOVERLAPPED overlapped) {
    const DWORD chunk = (std::min)(bytes, static_cast<DWORD>(2));
    return ::WriteFile(file, buffer, chunk, written, overlapped);
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
}

fs::path test_root() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    assert(length > 0 && length < MAX_PATH);
    return fs::path(buffer) / (L"foo_speaklyrics-log-sink-" +
        std::to_wstring(GetTickCount64()));
}

void test_privacy_labels() {
    const std::wstring text = L"第一句歌词";
    const std::wstring hidden = speaklyrics_log_privacy::private_text(text, false);
    assert(hidden.find(text) == std::wstring::npos);
    assert(hidden.find(L"哈希=") != std::wstring::npos);
    assert(speaklyrics_log_privacy::private_text(text, true) == text);
    const std::wstring path = L"D:\\Music\\歌词\\song.lrc";
    const std::wstring pathLabel = speaklyrics_log_privacy::path_label(path, false);
    assert(pathLabel.find(L"song.lrc") != std::wstring::npos);
    assert(pathLabel.find(L"D:\\Music") == std::wstring::npos);
}

void test_short_writes_are_completed(const fs::path& path) {
    speaklyrics_log_io::file_api api;
    api.write = &short_write;
    speaklyrics_log_io::sink sink(1024, api);
    const auto result = sink.append(path.wstring(), "abcdef", 1);
    assert(result.success);
    assert(read_file(path) == "abcdef");
}

void test_rotation_replaces_backup(const fs::path& path) {
    speaklyrics_log_io::sink sink(4);
    assert(sink.append(path.wstring(), "abc", 1).success);
    const auto result = sink.append(path.wstring(), "def", 2);
    assert(result.success);
    assert(read_file(path) == "def");
    assert(read_file(path.wstring() + L".old") == "abc");
}

void test_failed_rotation_keeps_existing_files(const fs::path& path) {
    speaklyrics_log_io::file_api api;
    api.move = &fail_move;
    speaklyrics_log_io::sink sink(4, api);
    assert(sink.append(path.wstring(), "abc", 1).success);
    const auto result = sink.append(path.wstring(), "def", 2);
    assert(result.success);
    assert(!result.notices.empty());
    assert(read_file(path) == "abcdef");
    assert(!fs::exists(path.wstring() + L".old"));
}

int main() {
    const fs::path root = test_root();
    std::error_code error;
    fs::create_directories(root, error);
    assert(!error);
    test_privacy_labels();
    test_short_writes_are_completed(root / L"short.log");
    test_rotation_replaces_backup(root / L"rotate.log");
    test_failed_rotation_keeps_existing_files(root / L"failed-rotate.log");
    fs::remove_all(root, error);
    assert(!error);
    return 0;
}
