#ifndef C1XZ_PLATFORM_EVENT_LOOP_H
#define C1XZ_PLATFORM_EVENT_LOOP_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace c1xz {

// Single-threaded task queue with timers. Replaces the FreeRTOS event group
// and xTaskNotify plumbing of the upstream Application main loop.
//
// Post() is safe from any thread; every callback runs on the thread that called
// Run(). That keeps the ported application logic single threaded exactly like
// upstream, while audio and network run on their own threads and hand work back
// through Post().
class EventLoop {
public:
    using Task = std::function<void()>;
    using TimerId = uint64_t;

    EventLoop() = default;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Queues a task. Returns false once the loop has been stopped, so callers
    // can tell the difference between "will run" and "dropped during shutdown".
    bool Post(Task task);

    // Runs `task` after `delay`. Cancel with CancelTimer().
    TimerId PostDelayed(std::chrono::milliseconds delay, Task task);

    // Runs `task` every `interval`, first run one interval from now.
    TimerId PostRepeating(std::chrono::milliseconds interval, Task task);

    // Safe to call from inside the timer's own callback.
    void CancelTimer(TimerId id);

    // Runs until Stop(). Must be called from one thread only.
    void Run();

    // Wakes Run() and makes it return once the current task finishes.
    void Stop();

    bool stopped() const;

private:
    struct Timer {
        std::chrono::steady_clock::time_point due;
        std::chrono::milliseconds interval;  // zero for one-shot
        TimerId id;
        Task task;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Task> queue_;
    std::multimap<std::chrono::steady_clock::time_point, Timer> timers_;
    std::vector<TimerId> cancelled_;
    TimerId next_timer_id_ = 1;
    bool stopped_ = false;

    TimerId AddTimer(std::chrono::milliseconds delay, std::chrono::milliseconds interval,
                     Task task);
    bool IsCancelled(TimerId id) const;
};

int64_t MonotonicMs();

}  // namespace c1xz

#endif  // C1XZ_PLATFORM_EVENT_LOOP_H
