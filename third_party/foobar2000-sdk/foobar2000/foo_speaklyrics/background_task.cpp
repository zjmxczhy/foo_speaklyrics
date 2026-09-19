#include "stdafx.h"

#include "background_task.h"
#include "speaklyrics_log.h"

#include <set>

namespace {

CRITICAL_SECTION g_tasks_lock;
INIT_ONCE g_tasks_lock_once = INIT_ONCE_STATIC_INIT;
std::set<speaklyrics_background_task_ptr,
    std::owner_less<speaklyrics_background_task_ptr>> g_tasks;
bool g_shutting_down = false;

void init_tasks_lock_once() {
    InitOnceExecuteOnce(&g_tasks_lock_once, [](PINIT_ONCE, PVOID, PVOID*) -> BOOL {
        InitializeCriticalSection(&g_tasks_lock);
        return TRUE;
    }, nullptr, nullptr);
}

class tasks_lock_guard {
public:
    tasks_lock_guard() {
        init_tasks_lock_once();
        EnterCriticalSection(&g_tasks_lock);
    }

    ~tasks_lock_guard() {
        LeaveCriticalSection(&g_tasks_lock);
    }

    tasks_lock_guard(const tasks_lock_guard&) = delete;
    tasks_lock_guard& operator=(const tasks_lock_guard&) = delete;
};

}

speaklyrics_background_task::speaklyrics_background_task(const wchar_t* name)
    : m_aborter(std::make_shared<foobar2000_io::abort_callback_impl>()),
      m_name(name ? name : L"未命名后台任务") {
}

foobar2000_io::abort_callback& speaklyrics_background_task::aborter() {
    return *m_aborter;
}

bool speaklyrics_background_task::is_aborted() const {
    return m_aborter->is_set();
}

void speaklyrics_background_task::cancel() {
    m_aborter->set();
}

const std::wstring& speaklyrics_background_task::name() const {
    return m_name;
}

void speaklyrics_background_task::retain_callback() {
    m_pending_callbacks.fetch_add(1, std::memory_order_acq_rel);
}

void speaklyrics_background_task::release_callback() {
    m_pending_callbacks.fetch_sub(1, std::memory_order_acq_rel);
    remove_if_finished();
}

void speaklyrics_background_task::finish_worker() {
    m_worker_finished.store(true, std::memory_order_release);
    remove_if_finished();
}

void speaklyrics_background_task::remove_if_finished() {
    if (!m_worker_finished.load(std::memory_order_acquire) ||
        m_pending_callbacks.load(std::memory_order_acquire) != 0) {
        return;
    }

    tasks_lock_guard guard;
    g_tasks.erase(shared_from_this());
}

bool speaklyrics_background_task::post_to_main_thread(std::function<void()> callback) {
    if (!callback || is_aborted()) return false;

    retain_callback();
    const auto self = shared_from_this();
    auto callbackLifetime = std::shared_ptr<void>(nullptr, [self](void*) {
        self->release_callback();
    });

    try {
        fb2k::inMainThread(
            [self, callbackLifetime, callback = std::move(callback)]() mutable {
                if (self->is_aborted()) return;
                try {
                    callback();
                } catch (...) {
                    speaklyrics_log_error(L"后台任务主线程回调发生异常：%s。", self->name().c_str());
                }
            },
            aborter());
    } catch (...) {
        // callbackLifetime releases the pending callback reference here.
        return false;
    }
    return true;
}

speaklyrics_background_task_ptr speaklyrics_start_background_task(const wchar_t* name) {
    tasks_lock_guard guard;
    if (g_shutting_down) return nullptr;

    speaklyrics_background_task_ptr task(new speaklyrics_background_task(name));
    g_tasks.insert(task);
    return task;
}

bool speaklyrics_run_background_task(const speaklyrics_background_task_ptr& task,
    std::function<void(speaklyrics_background_task&)> work) {
    if (!task || !work) return false;

    try {
        fb2k::splitTask([task, work = std::move(work)]() mutable {
            if (!task->is_aborted()) {
                try {
                    work(*task);
                } catch (const foobar2000_io::exception_aborted&) {
                    speaklyrics_log_info(L"后台任务已取消：%s。", task->name().c_str());
                } catch (const std::exception&) {
                    speaklyrics_log_error(L"后台任务发生异常：%s。", task->name().c_str());
                } catch (...) {
                    speaklyrics_log_error(L"后台任务发生未知异常：%s。", task->name().c_str());
                }
            }
            task->finish_worker();
        });
        return true;
    } catch (...) {
        task->finish_worker();
        speaklyrics_log_error(L"后台任务无法启动：%s。", task->name().c_str());
        return false;
    }
}

void speaklyrics_cancel_all_background_tasks() {
    std::vector<speaklyrics_background_task_ptr> tasks;
    {
        tasks_lock_guard guard;
        g_shutting_down = true;
        tasks.assign(g_tasks.begin(), g_tasks.end());
    }

    for (const auto& task : tasks) task->cancel();
}
