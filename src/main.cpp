#include "server.hpp"
#include "store.hpp"
#include "thread_pool.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

// The signal handler runs in async-signal context and cannot safely close
// over local state, so the server pointer lives in a small atomic global.
// load/store on a lock-free atomic<T*> is async-signal-safe on every platform
// we'd run this on, and Server::request_shutdown itself only does an atomic
// exchange and a close() — both on the POSIX async-signal-safe list.
std::atomic<Server*> g_server{nullptr};

extern "C" void on_signal(int) {
    if (auto* s = g_server.load(std::memory_order_acquire)) {
        s->request_shutdown();
    }
}

void install_signal(int sig, void (*handler)(int)) {
    struct sigaction sa{};
    sa.sa_handler = handler;
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (::sigaction(sig, &sa, nullptr) < 0) {
        throw std::system_error(errno, std::generic_category(), "sigaction");
    }
}

template <typename T>
T parse_int(std::string_view arg, const char* name) {
    T out{};
    const auto* begin = arg.data();
    const auto* end = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument(std::string("invalid ") + name + ": '"
                                    + std::string(arg) + "'");
    }
    return out;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [port [shards [threads]]]\n"
              << "  port     listen port           (default 6379)\n"
              << "  shards   store shard count     (default 16)\n"
              << "  threads  worker thread count   (default hardware_concurrency())\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::uint16_t port = 6379;
        std::size_t shards = 16;
        std::size_t threads = std::thread::hardware_concurrency();
        if (threads == 0) threads = 4;

        if (argc > 1) port    = parse_int<std::uint16_t>(argv[1], "port");
        if (argc > 2) shards  = parse_int<std::size_t>(argv[2], "shards");
        if (argc > 3) threads = parse_int<std::size_t>(argv[3], "threads");
        if (argc > 4) { print_usage(argv[0]); return 2; }

        // Ignore SIGPIPE so a peer disconnecting mid-write doesn't terminate
        // the process. send() with MSG_NOSIGNAL covers the socket path; this
        // covers anything else (logging, future stdout/stderr writes).
        install_signal(SIGPIPE, SIG_IGN);

        // Declaration order is destruction order in reverse: server is built
        // last so it's destroyed first. Server::run() blocks on its in-flight
        // counter so all handlers are done before run() returns, which means
        // by the time ~Server starts there's nobody left to touch `this`.
        // Then ThreadPool can join its workers; then Store goes away.
        Store store(shards);
        ThreadPool pool(threads);
        Server server(store, pool, port);

        g_server.store(&server, std::memory_order_release);
        install_signal(SIGINT, on_signal);
        install_signal(SIGTERM, on_signal);

        std::cout << "kvstore-server listening on :" << port
                  << " shards=" << shards
                  << " threads=" << pool.size() << "\n";

        server.run();

        g_server.store(nullptr, std::memory_order_release);
        std::cout << "kvstore-server: shutdown complete\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
