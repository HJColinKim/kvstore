#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

class Store;
class ThreadPool;

class Server {
public:
    Server(Store& store, ThreadPool& pool, std::uint16_t port);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Accept loop. Returns only after the listen socket has closed AND every
    // in-flight connection handler has completed, so the caller can safely
    // destroy this Server (and its referenced Store / ThreadPool) on return.
    void run();

    // Idempotent. Closes the listen socket so a blocked accept() returns
    // EBADF and run()'s loop exits. Safe to call from a signal handler
    // because close() is on the POSIX async-signal-safe list and the only
    // other operation is an atomic exchange.
    void request_shutdown() noexcept;

private:
    void handle_connection(int client_fd);

    Store& store_;
    ThreadPool& pool_;
    std::uint16_t port_;

    int listen_fd_;
    std::atomic<bool> shutting_down_{false};

    // Connection-handler refcount. The accept loop knows when it has stopped
    // accepting; this counter tells run() when the handlers it already
    // dispatched are done, which is the moment it's safe to return. Without
    // this, handlers (which capture `this`) could outlive the Server.
    std::atomic<std::size_t> in_flight_{0};
    std::mutex in_flight_mtx_;
    std::condition_variable in_flight_cv_;
};
