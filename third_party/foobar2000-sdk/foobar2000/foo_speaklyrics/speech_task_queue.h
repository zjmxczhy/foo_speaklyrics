#pragma once

#include "speech_task.h"

#include <algorithm>
#include <deque>
#include <limits>
#include <vector>

class speech_task_queue {
public:
    static constexpr size_t default_capacity = 32;

    explicit speech_task_queue(size_t capacity = default_capacity)
        : m_capacity((std::max)(size_t{ 1 }, capacity)) {}

    speech_enqueue_status enqueue(speech_task task, uint64_t now,
        std::vector<speech_task>& expired,
        std::vector<speech_task_cancellation>& canceled) {
        return enqueue_impl(std::move(task), now, expired, canceled, false);
    }

    speech_enqueue_status requeue_retry(speech_task task, uint64_t now,
        std::vector<speech_task>& expired,
        std::vector<speech_task_cancellation>& canceled) {
        return enqueue_impl(std::move(task), now, expired, canceled, true);
    }

    bool take_ready(uint64_t now, speech_task& task, std::vector<speech_task>& expired) {
        purge_expired(now, expired);

        auto selected = m_tasks.end();
        int selectedPriority = (std::numeric_limits<int>::max)();
        for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
            const int priority = task_priority(*it);
            if (selected == m_tasks.end() || priority < selectedPriority) {
                selected = it;
                selectedPriority = priority;
            }
        }
        if (selected == m_tasks.end()) return false;
        // The first task at the highest available priority owns the queue.
        // In particular, a delayed retry keeps later lyrics from passing it.
        if (selected->ready_at != 0 && selected->ready_at > now) return false;

        task = std::move(*selected);
        m_tasks.erase(selected);
        return true;
    }

    uint64_t next_wake_at() const {
        auto selected = m_tasks.end();
        int selectedPriority = (std::numeric_limits<int>::max)();
        for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
            const int priority = task_priority(*it);
            if (selected == m_tasks.end() || priority < selectedPriority) {
                selected = it;
                selectedPriority = priority;
            }
        }
        if (selected == m_tasks.end() || selected->ready_at == 0) return 0;
        uint64_t wakeAt = selected->ready_at;
        if (selected->expires_at != 0 && selected->expires_at < wakeAt) {
            wakeAt = selected->expires_at;
        }
        return wakeAt;
    }

    void invalidate_before_generation(uint64_t activeGeneration,
        speech_invalidation_reason reason,
        std::vector<speech_task_cancellation>& canceled) {
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (it->playback_generation < activeGeneration) {
                canceled.push_back({ std::move(*it), reason });
                it = m_tasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    void cancel_all(speech_invalidation_reason reason,
        std::vector<speech_task_cancellation>& canceled) {
        while (!m_tasks.empty()) {
            canceled.push_back({ std::move(m_tasks.front()), reason });
            m_tasks.pop_front();
        }
    }

    bool remove(uint64_t taskId, speech_task* removed = nullptr) {
        const auto found = std::find_if(m_tasks.begin(), m_tasks.end(),
            [taskId](const speech_task& task) { return task.task_id == taskId; });
        if (found == m_tasks.end()) return false;
        if (removed) *removed = std::move(*found);
        m_tasks.erase(found);
        return true;
    }

    size_t size() const { return m_tasks.size(); }
    bool empty() const { return m_tasks.empty(); }
    size_t capacity() const { return m_capacity; }

private:
    speech_enqueue_status enqueue_impl(speech_task task, uint64_t now,
        std::vector<speech_task>& expired,
        std::vector<speech_task_cancellation>& canceled,
        bool retryAtFront) {
        purge_expired(now, expired);
        if (speech_task_is_expired(task, now)) return speech_enqueue_status::expired;

        if (task.type == speech_task_type::lyric) {
            cancel_lower_priority_tasks(task_priority(task),
                speech_invalidation_reason::superseded_by_lyric, canceled);
        }

        while (m_tasks.size() >= m_capacity) {
            auto candidate = find_worst_lower_priority(task_priority(task));
            if (candidate == m_tasks.end()) return speech_enqueue_status::queue_full;
            canceled.push_back({ std::move(*candidate), speech_invalidation_reason::queue_pressure });
            m_tasks.erase(candidate);
        }

        if (retryAtFront) m_tasks.push_front(std::move(task));
        else m_tasks.push_back(std::move(task));
        return speech_enqueue_status::accepted;
    }

    static int task_priority(const speech_task& task) {
        switch (task.type) {
        case speech_task_type::lyric:
            return 0;
        case speech_task_type::track_announcement:
        case speech_task_type::auto_speak_enabled:
        case speech_task_type::auto_speak_disabled:
            return 1;
        case speech_task_type::general:
            return 2;
        case speech_task_type::preload:
        default:
            return 3;
        }
    }

    void purge_expired(uint64_t now, std::vector<speech_task>& expired) {
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (speech_task_is_expired(*it, now)) {
                expired.push_back(std::move(*it));
                it = m_tasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    void cancel_lower_priority_tasks(int incomingPriority,
        speech_invalidation_reason reason,
        std::vector<speech_task_cancellation>& canceled) {
        for (auto it = m_tasks.begin(); it != m_tasks.end();) {
            if (task_priority(*it) > incomingPriority) {
                canceled.push_back({ std::move(*it), reason });
                it = m_tasks.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::deque<speech_task>::iterator find_worst_lower_priority(int incomingPriority) {
        auto selected = m_tasks.end();
        int selectedPriority = incomingPriority;
        for (auto it = m_tasks.begin(); it != m_tasks.end(); ++it) {
            const int priority = task_priority(*it);
            if (priority > selectedPriority) {
                selected = it;
                selectedPriority = priority;
            }
        }
        return selected;
    }

    size_t m_capacity;
    std::deque<speech_task> m_tasks;
};
