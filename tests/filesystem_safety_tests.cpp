#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/filesystem_safety.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>


namespace fs = std::filesystem;

namespace {

fs::path make_test_folder() {
    std::error_code error;
    const fs::path root = fs::temp_directory_path(error);
    assert(!error);
    const auto suffix = std::chrono::high_resolution_clock::now()
        .time_since_epoch().count();
    return root / (L"foo_speaklyrics-filesystem-safety-" +
        std::to_wstring(static_cast<long long>(suffix)));
}

void test_safe_path_queries_and_directory_iteration() {
    const fs::path folder = make_test_folder();
    const fs::path file = folder / L"sample.lrc";
    const fs::path missing = folder / L"missing.lrc";

    std::error_code error;
    fs::create_directories(folder, error);
    assert(!error);
    {
        std::ofstream output(file, std::ios::binary);
        output << "[00:01.00]line\n";
    }

    const auto folderResult = speaklyrics_filesystem::is_directory(folder);
    assert(folderResult.value);
    assert(!folderResult.error);

    const auto fileResult = speaklyrics_filesystem::is_regular_file(file);
    assert(fileResult.value);
    assert(!fileResult.error);

    const auto missingResult = speaklyrics_filesystem::exists(missing);
    assert(!missingResult.value);
    assert(!missingResult.error);

    fs::directory_iterator iterator;
    error = speaklyrics_filesystem::open_directory(folder, iterator);
    assert(!error);

    size_t entries = 0;
    const fs::directory_iterator end;
    while (iterator != end) {
        ++entries;
        error = speaklyrics_filesystem::increment_directory(iterator);
        assert(!error);
    }
    assert(entries == 1);

    fs::remove_all(folder, error);
    assert(!error);
}

void test_missing_directory_reports_error_without_throwing() {
    const fs::path folder = make_test_folder();
    fs::directory_iterator iterator;
    const std::error_code error =
        speaklyrics_filesystem::open_directory(folder, iterator);
    assert(error);
    assert(iterator == fs::directory_iterator{});
}

} // namespace

int main() {
    test_safe_path_queries_and_directory_iteration();
    test_missing_directory_reports_error_without_throwing();
    std::cout << "filesystem_safety tests passed" << std::endl;
    return 0;
}
