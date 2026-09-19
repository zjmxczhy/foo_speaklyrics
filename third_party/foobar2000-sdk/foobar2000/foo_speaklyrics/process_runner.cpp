#include "stdafx.h"

#include "process_runner.h"

#include <algorithm>

namespace {

class unique_handle {
public:
    unique_handle() = default;
    explicit unique_handle(HANDLE handle) : m_handle(handle) {}
    ~unique_handle() { reset(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept : m_handle(other.release()) {}
    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const { return m_handle; }
    explicit operator bool() const {
        return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE;
    }

    HANDLE release() {
        HANDLE result = m_handle;
        m_handle = nullptr;
        return result;
    }

    void reset(HANDLE handle = nullptr) {
        if (*this) CloseHandle(m_handle);
        m_handle = handle;
    }

private:
    HANDLE m_handle = nullptr;
};

enum class pipe_read_status {
    idle,
    available,
    closed,
    error,
    output_limit,
};

pipe_read_status drain_pipe(HANDLE pipe, std::string& output,
    size_t maximumOutputBytes, DWORD& errorCode) {
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            errorCode = GetLastError();
            if (errorCode == ERROR_BROKEN_PIPE || errorCode == ERROR_PIPE_NOT_CONNECTED) {
                return pipe_read_status::closed;
            }
            if (errorCode == ERROR_NO_DATA) return pipe_read_status::idle;
            return pipe_read_status::error;
        }
        if (available == 0) return pipe_read_status::idle;

        char buffer[4096];
        const DWORD requested = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
        DWORD read = 0;
        if (!ReadFile(pipe, buffer, requested, &read, nullptr)) {
            errorCode = GetLastError();
            if (errorCode == ERROR_BROKEN_PIPE || errorCode == ERROR_PIPE_NOT_CONNECTED) {
                return pipe_read_status::closed;
            }
            return pipe_read_status::error;
        }
        if (read == 0) return pipe_read_status::idle;

        const size_t currentSize = output.size();
        if (currentSize >= maximumOutputBytes ||
            static_cast<size_t>(read) > maximumOutputBytes - currentSize) {
            const size_t remaining = currentSize < maximumOutputBytes
                ? maximumOutputBytes - currentSize : 0;
            if (remaining > 0) output.append(buffer, remaining);
            return pipe_read_status::output_limit;
        }

        output.append(buffer, read);
    }
}

unique_handle create_job() {
    unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) return {};

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        return {};
    }
    return job;
}

void terminate_process_tree(HANDLE process, HANDLE job) {
    if (job == nullptr || !TerminateJobObject(job, 1)) {
        TerminateProcess(process, 1);
    }
    // The wait is deliberately bounded. The parent never waits forever on a
    // broken downloader, and closing the job handle still kills its children.
    WaitForSingleObject(process, 2000);
}

}

speaklyrics_process_result run_process_capture_stdout(
    const std::wstring& command,
    const std::filesystem::path& workDir,
    foobar2000_io::abort_callback& aborter,
    DWORD timeoutMs,
    size_t maximumOutputBytes) {
    speaklyrics_process_result result;
    result.output.reserve((std::min)(maximumOutputBytes, static_cast<size_t>(64 * 1024)));

    if (command.empty()) return result;
    if (aborter.is_aborting()) {
        result.status = speaklyrics_process_status::aborted;
        return result;
    }

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipeHandle = nullptr;
    HANDLE writePipeHandle = nullptr;
    if (!CreatePipe(&readPipeHandle, &writePipeHandle, &security, 0)) {
        result.error_code = GetLastError();
        return result;
    }
    unique_handle readPipe(readPipeHandle);
    unique_handle writePipe(writePipeHandle);
    if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        result.error_code = GetLastError();
        return result;
    }

    unique_handle input(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input) {
        result.error_code = GetLastError();
        return result;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = input.get();
    startup.hStdOutput = writePipe.get();
    startup.hStdError = writePipe.get();

    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    PROCESS_INFORMATION processInfo = {};
    const wchar_t* currentDirectory = workDir.empty() ? nullptr : workDir.c_str();
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, currentDirectory,
        &startup, &processInfo);

    // These handles must not remain open in the parent, otherwise the pipe
    // cannot reach EOF after the child exits.
    writePipe.reset();
    input.reset();

    if (!created) {
        result.error_code = GetLastError();
        return result;
    }

    unique_handle process(processInfo.hProcess);
    unique_handle thread(processInfo.hThread);
    unique_handle job = create_job();
    if (job && !AssignProcessToJobObject(job.get(), process.get())) {
        job.reset();
    }

    result.status = speaklyrics_process_status::completed;
    const ULONGLONG startedAt = GetTickCount64();
    bool terminate = false;

    for (;;) {
        if (aborter.is_aborting()) {
            result.status = speaklyrics_process_status::aborted;
            terminate = true;
            break;
        }
        if (timeoutMs > 0 && GetTickCount64() - startedAt >= timeoutMs) {
            result.status = speaklyrics_process_status::timed_out;
            terminate = true;
            break;
        }

        DWORD pipeError = ERROR_SUCCESS;
        const pipe_read_status pipeStatus = drain_pipe(
            readPipe.get(), result.output, maximumOutputBytes, pipeError);
        if (pipeStatus == pipe_read_status::output_limit) {
            result.status = speaklyrics_process_status::output_limit;
            result.error_code = ERROR_INSUFFICIENT_BUFFER;
            terminate = true;
            break;
        }
        if (pipeStatus == pipe_read_status::error) {
            result.status = speaklyrics_process_status::pipe_error;
            result.error_code = pipeError;
            terminate = true;
            break;
        }

        const DWORD processWait = WaitForSingleObject(process.get(), 0);
        if (processWait == WAIT_OBJECT_0) {
            // The process is already gone, so only consume bytes currently
            // buffered in the pipe. Never wait for EOF here.
            for (;;) {
                DWORD drainError = ERROR_SUCCESS;
                const pipe_read_status drainStatus = drain_pipe(
                    readPipe.get(), result.output, maximumOutputBytes, drainError);
                if (drainStatus == pipe_read_status::output_limit) {
                    result.status = speaklyrics_process_status::output_limit;
                    result.error_code = ERROR_INSUFFICIENT_BUFFER;
                    break;
                }
                if (drainStatus != pipe_read_status::available) break;
            }
            break;
        }
        if (processWait == WAIT_FAILED) {
            result.status = speaklyrics_process_status::pipe_error;
            result.error_code = GetLastError();
            terminate = true;
            break;
        }

        WaitForSingleObject(aborter.get_abort_event(), 25);
    }

    if (terminate) {
        terminate_process_tree(process.get(), job.get());
    } else {
        // The process was observed as signaled, so this is non-blocking in
        // normal operation and still has a finite bound if state changed.
        WaitForSingleObject(process.get(), 2000);
    }

    if (!GetExitCodeProcess(process.get(), &result.exit_code)) {
        result.exit_code = 3;
        if (result.error_code == ERROR_SUCCESS) result.error_code = GetLastError();
    }
    return result;
}

const wchar_t* speaklyrics_process_status_name(speaklyrics_process_status status) {
    switch (status) {
    case speaklyrics_process_status::completed: return L"已完成";
    case speaklyrics_process_status::failed_to_start: return L"启动失败";
    case speaklyrics_process_status::aborted: return L"已取消";
    case speaklyrics_process_status::timed_out: return L"超时";
    case speaklyrics_process_status::output_limit: return L"输出超限";
    case speaklyrics_process_status::pipe_error: return L"管道错误";
    default: return L"未知";
    }
}
