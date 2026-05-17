#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <stop_token>
#include <thread>
#include <vector>

class ThreadPool {
public:
    // Defaults to hardware_concurrency; falls back to 1 if that returns 0
    // (which is allowed by the standard when the value cannot be determined).
    explicit ThreadPool(std::size_t threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a task. Throws std::runtime_error if the pool is shutting down.
    // Tasks already in the queue at shutdown are drained (not discarded),
    // which keeps connection-handler semantics simple: once the server
    // submits a handler, it will run to completion.
    void submit(std::function<void()> task);

    std::size_t size() const noexcept { return workers_.size(); }

private:
    void worker_loop(std::stop_token st);

    std::mutex mtx_;
    // condition_variable_any (rather than condition_variable) is required for
    // the wait(lock, stop_token, predicate) overload, which registers a
    // stop_callback so that request_stop() can wake a blocked worker without
    // a separate notify_all.
    std::condition_variable_any cv_;
    std::queue<std::function<void()>> tasks_;
    bool stopped_ = false;

    // workers_ MUST be declared last: members are destroyed in reverse
    // declaration order, so the vector of jthreads is torn down first. Each
    // jthread's destructor calls request_stop() then join(). The queue,
    // mutex, and cv outlive the workers, so a draining worker can still
    // safely touch them.
    std::vector<std::jthread> workers_;
};
