#pragma once

#include <SDK/abort_callback.h>

#include <Windows.h>

#include <filesystem>
#include <string>

enum class speaklyrics_process_status {
    completed,
    failed_to_start,
    aborted,
    timed_out,
    output_limit,
    pipe_error,
};

struct speaklyrics_process_result {
    speaklyrics_process_status status = speaklyrics_process_status::failed_to_start;
    DWORD exit_code = 3;
    DWORD error_code = ERROR_SUCCESS;
    std::string output;
};

speaklyrics_process_result run_process_capture_stdout(
    const std::wstring& command,
    const std::filesystem::path& workDir,
    foobar2000_io::abort_callback& aborter,
    DWORD timeoutMs,
    size_t maximumOutputBytes);

const wchar_t* speaklyrics_process_status_name(speaklyrics_process_status status);
