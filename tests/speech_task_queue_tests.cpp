#include "../third_party/foobar2000-sdk/foobar2000/foo_speaklyrics/speech_task_queue.h"

#include <cassert>
#include <iostream>
#include <vector>

namespace {

speech_task make_task(uint64_t id, speech_task_type type, uint64_t readyAt = 0,
    uint64_t expiresAt = 10000, uint64_t generation = 1) {
    speech_task task;
    task.task_id = id;
    task.type = type;
    task.text = L"test";
    task.ready_at = readyAt;
    task.expires_at = expiresAt;
    task.playback_generation = generation;
    return task;
}

void expect_enqueue(speech_task_queue& queue, speech_task task, uint64_t now,
    speech_enqueue_status expected) {
    std::vector<speech_task> expired;
    std::vector<speech_task_cancellation> canceled;
    assert(queue.enqueue(std::move(task), now, expired, canceled) == expected);
    assert(expired.empty());
    assert(canceled.empty());
}

void test_lyric_fifo() {
    speech_task_queue queue;
    expect_enqueue(queue, make_task(1, speech_task_type::lyric), 0,
        speech_enqueue_status::accepted);
    expect_enqueue(queue, make_task(2, speech_task_type::lyric), 0,
        speech_enqueue_status::accepted);

    std::vector<speech_task> expired;
    speech_task task;
    assert(queue.take_ready(0, task, expired));
    assert(task.task_id == 1);
    assert(queue.take_ready(0, task, expired));
    assert(task.task_id == 2);
}

void test_retry_keeps_fifo_position() {
    speech_task_queue queue;
    expect_enqueue(queue, make_task(1, speech_task_type::lyric), 0,
        speech_enqueue_status::accepted);

    std::vector<speech_task> expired;
    speech_task first;
    assert(queue.take_ready(0, first, expired));
    expect_enqueue(queue, make_task(2, speech_task_type::lyric), 0,
        speech_enqueue_status::accepted);

    first.ready_at = 100;
    std::vector<speech_task_cancellation> canceled;
    assert(queue.requeue_retry(first, 0, expired, canceled) ==
        speech_enqueue_status::accepted);
    assert(!queue.take_ready(50, first, expired));
    assert(queue.next_wake_at() == 100);
    assert(queue.take_ready(100, first, expired));
    assert(first.task_id == 1);
    assert(queue.take_ready(100, first, expired));
    assert(first.task_id == 2);
}

void test_default_capacity_rejects_33rd_lyric() {
    speech_task_queue queue;
    assert(queue.capacity() == 32);
    for (uint64_t id = 1; id <= 32; ++id) {
        expect_enqueue(queue, make_task(id, speech_task_type::lyric), 0,
            speech_enqueue_status::accepted);
    }

    std::vector<speech_task> expired;
    std::vector<speech_task_cancellation> canceled;
    assert(queue.enqueue(make_task(33, speech_task_type::lyric), 0, expired, canceled) ==
        speech_enqueue_status::queue_full);
    assert(queue.size() == 32);
    assert(canceled.empty());
}

void test_higher_priority_can_evict_lower_priority() {
    speech_task_queue queue(2);
    expect_enqueue(queue, make_task(1, speech_task_type::general), 0,
        speech_enqueue_status::accepted);
    expect_enqueue(queue, make_task(2, speech_task_type::track_announcement), 0,
        speech_enqueue_status::accepted);

    std::vector<speech_task> expired;
    std::vector<speech_task_cancellation> canceled;
    assert(queue.enqueue(make_task(3, speech_task_type::track_announcement), 0,
        expired, canceled) == speech_enqueue_status::accepted);
    assert(canceled.size() == 1);
    assert(canceled.front().task.task_id == 1);
    assert(canceled.front().reason == speech_invalidation_reason::queue_pressure);
}

void test_generation_invalidation() {
    speech_task_queue queue;
    expect_enqueue(queue, make_task(1, speech_task_type::lyric, 0, 10000, 1), 0,
        speech_enqueue_status::accepted);
    expect_enqueue(queue, make_task(2, speech_task_type::lyric, 0, 10000, 2), 0,
        speech_enqueue_status::accepted);
    expect_enqueue(queue, make_task(3, speech_task_type::lyric, 0, 10000, 3), 0,
        speech_enqueue_status::accepted);

    std::vector<speech_task_cancellation> canceled;
    queue.invalidate_before_generation(3, speech_invalidation_reason::playback_seek, canceled);
    assert(canceled.size() == 2);
    assert(canceled[0].task.task_id == 1);
    assert(canceled[1].task.task_id == 2);
    assert(queue.size() == 1);

    std::vector<speech_task> expired;
    speech_task task;
    assert(queue.take_ready(0, task, expired));
    assert(task.task_id == 3);
}

} // namespace

int main() {
    test_lyric_fifo();
    test_retry_keeps_fifo_position();
    test_default_capacity_rejects_33rd_lyric();
    test_higher_priority_can_evict_lower_priority();
    test_generation_invalidation();
    std::cout << "speech_task_queue tests passed\n";
    return 0;
}
