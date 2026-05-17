#include "thread_pool.hpp"

#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(std::size_t threads) {
    if (threads == 0) threads = 1;
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        // jthread passes its own stop_token as the first argument to the
        // worker callable; capturing `this` lets the worker call the
        // private worker_loop member.
        workers_.emplace_back(
            [this](std::stop_token st) { worker_loop(st); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mtx_);
        stopped_ = true;
    }
    // We deliberately do NOT call notify_all or request_stop here. The
    // workers_ vector is destroyed first by virtue of declaration order;
    // each jthread's destructor handles request_stop + join, and the
    // stop_callback wired into cv_.wait wakes any worker blocked on the
    // empty queue.
}

void ThreadPool::submit(std::function<void()> task) {
    {
        std::lock_guard lock(mtx_);
        if (stopped_) {
            throw std::runtime_error("ThreadPool: submit after shutdown");
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThreadPool::worker_loop(std::stop_token st) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mtx_);
            // wait returns true if predicate became true; returns false if
            // stop was requested and the predicate is still false. So a
            // non-empty queue is processed even after stop is requested,
            // and an empty queue + stop is the only exit path.
            cv_.wait(lock, st, [this] { return !tasks_.empty(); });
            if (tasks_.empty()) return;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (...) {
            // A task that escapes its own exception handling must not be
            // allowed to terminate the worker thread, which would silently
            // shrink the pool's capacity. The task is responsible for any
            // user-visible error reporting; we just keep the worker alive.
        }
    }
}
