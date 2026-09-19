#pragma once

#include <SDK/abort_callback.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

// A small lifetime wrapper around fb2k::splitTask(). The SDK owns the actual
// worker until shutdown, while this object supplies per-operation cancellation
// and guards callbacks that are posted back to the main thread.
class speaklyrics_background_task : public std::enable_shared_from_this<speaklyrics_background_task> {
public:
    using ptr = std::shared_ptr<speaklyrics_background_task>;

    foobar2000_io::abort_callback& aborter();
    bool is_aborted() const;
    void cancel();
    bool post_to_main_thread(std::function<void()> callback);
    const std::wstring& name() const;

private:
    friend ptr speaklyrics_start_background_task(const wchar_t* name);
    friend bool speaklyrics_run_background_task(const ptr& task,
        std::function<void(speaklyrics_background_task&)> work);

    explicit speaklyrics_background_task(const wchar_t* name);

    void finish_worker();
    void retain_callback();
    void release_callback();
    void remove_if_finished();

    std::shared_ptr<foobar2000_io::abort_callback_impl> m_aborter;
    std::wstring m_name;
    std::atomic_bool m_worker_finished{ false };
    std::atomic_uint m_pending_callbacks{ 0 };
};

using speaklyrics_background_task_ptr = speaklyrics_background_task::ptr;

speaklyrics_background_task_ptr speaklyrics_start_background_task(const wchar_t* name);
bool speaklyrics_run_background_task(const speaklyrics_background_task_ptr& task,
    std::function<void(speaklyrics_background_task&)> work);
void speaklyrics_cancel_all_background_tasks();
