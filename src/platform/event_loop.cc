#include "platform/event_loop.h"

#include <algorithm>
#include <ctime>

namespace c1xz {

int64_t MonotonicMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool EventLoop::Post(Task task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return false;
        }
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

EventLoop::TimerId EventLoop::AddTimer(std::chrono::milliseconds delay,
                                       std::chrono::milliseconds interval, Task task) {
    if (!task) {
        return 0;
    }
    TimerId id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) {
            return 0;
        }
        id = next_timer_id_++;
        Timer timer;
        timer.due = std::chrono::steady_clock::now() + delay;
        timer.interval = interval;
        timer.id = id;
        timer.task = std::move(task);
        timers_.emplace(timer.due, std::move(timer));
    }
    cv_.notify_one();
    return id;
}

EventLoop::TimerId EventLoop::PostDelayed(std::chrono::milliseconds delay, Task task) {
    return AddTimer(delay, std::chrono::milliseconds::zero(), std::move(task));
}

EventLoop::TimerId EventLoop::PostRepeating(std::chrono::milliseconds interval, Task task) {
    return AddTimer(interval, interval, std::move(task));
}

void EventLoop::CancelTimer(TimerId id) {
    if (id == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.id == id) {
            timers_.erase(it);
            return;
        }
    }
    // The timer is currently running: remember the cancellation so a repeating
    // timer is not rescheduled after its callback returns.
    cancelled_.push_back(id);
}

bool EventLoop::IsCancelled(TimerId id) const {
    return std::find(cancelled_.begin(), cancelled_.end(), id) != cancelled_.end();
}

void EventLoop::Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_) {
        // Timers first so a busy task queue cannot starve them.
        auto now = std::chrono::steady_clock::now();
        if (!timers_.empty() && timers_.begin()->first <= now) {
            Timer timer = std::move(timers_.begin()->second);
            timers_.erase(timers_.begin());
            lock.unlock();
            timer.task();
            lock.lock();
            if (timer.interval > std::chrono::milliseconds::zero() && !stopped_) {
                if (IsCancelled(timer.id)) {
                    cancelled_.erase(std::remove(cancelled_.begin(), cancelled_.end(), timer.id),
                                     cancelled_.end());
                } else {
                    // Schedule from now, not from the original due time, so a
                    // slow callback cannot build up a backlog of firings.
                    timer.due = std::chrono::steady_clock::now() + timer.interval;
                    timers_.emplace(timer.due, std::move(timer));
                }
            }
            continue;
        }

        if (!queue_.empty()) {
            std::vector<Task> batch;
            batch.swap(queue_);
            lock.unlock();
            for (auto& task : batch) {
                task();
            }
            lock.lock();
            continue;
        }

        if (timers_.empty()) {
            cv_.wait(lock);
        } else {
            cv_.wait_until(lock, timers_.begin()->first);
        }
    }
}

void EventLoop::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    cv_.notify_all();
}

bool EventLoop::stopped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
}

}  // namespace c1xz
