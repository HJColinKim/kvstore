#include "server.hpp"

#include "protocol.hpp"
#include "store.hpp"
#include "thread_pool.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <variant>

namespace {

std::optional<std::string> read_line(int fd, std::string& buf,
                                     std::size_t max_line) {
    // Pipelined requests can leave more than one line in the buffer; check it
    // before issuing another recv. A line is returned with its trailing '\n'
    // already removed.
    while (true) {
        if (auto nl = buf.find('\n'); nl != std::string::npos) {
            std::string line(buf, 0, nl);
            buf.erase(0, nl + 1);
            return line;
        }
        if (buf.size() >= max_line) {
            return std::nullopt;     // refuse to grow the buffer further
        }

        constexpr std::size_t chunk = 4096;
        char tmp[chunk];
        ssize_t n;
        do {
            n = ::recv(fd, tmp, chunk, 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) return std::nullopt;     // EOF or unrecoverable error
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n;
        do {
            // MSG_NOSIGNAL: surface broken pipes as EPIPE instead of raising
            // SIGPIPE (which would otherwise kill the process by default).
            // We also SIG_IGN SIGPIPE in main as belt-and-braces.
            n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::string dispatch(Store& store, const Command& cmd) {
    switch (cmd.verb) {
    case Verb::SET:
        store.set(cmd.key, cmd.value);
        return respond_ok();
    case Verb::GET: {
        auto v = store.get(cmd.key);
        return v ? respond_value(*v) : respond_not_found();
    }
    case Verb::DEL:
        return store.del(cmd.key) ? respond_ok() : respond_not_found();
    case Verb::EXISTS:
        return store.exists(cmd.key) ? respond_ok() : respond_not_found();
    case Verb::EXPIRE:
        return store.expire(cmd.key, cmd.ttl) ? respond_ok()
                                              : respond_not_found();
    case Verb::KEYS: {
        // Buffer the whole reply rather than streaming. For pathologically
        // large keyspaces this could be a few MB; acceptable for the project
        // scope and avoids interleaving writes with other handler work.
        auto keys = store.keys();
        std::string out = respond_count(keys.size());
        for (const auto& k : keys) out += respond_key(k);
        return out;
    }
    }
    return respond_error("internal: unknown verb");
}

}  // namespace

Server::Server(Store& store, ThreadPool& pool, std::uint16_t port)
    : store_(store), pool_(pool), port_(port), listen_fd_(-1) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    // SO_REUSEADDR lets the server re-bind immediately after a quick restart
    // instead of waiting for TIME_WAIT to expire on the previous socket.
    int yes = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes,
                     sizeof(yes)) < 0) {
        const int e = errno;
        ::close(listen_fd_);
        throw std::system_error(e, std::generic_category(),
                                "setsockopt SO_REUSEADDR");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
               sizeof(addr)) < 0) {
        const int e = errno;
        ::close(listen_fd_);
        throw std::system_error(e, std::generic_category(),
                                std::string("bind :") + std::to_string(port_));
    }

    if (::listen(listen_fd_, SOMAXCONN) < 0) {
        const int e = errno;
        ::close(listen_fd_);
        throw std::system_error(e, std::generic_category(), "listen");
    }
}

Server::~Server() {
    // Idempotent with request_shutdown via the exchange: whichever path runs
    // first closes the fd; the other is a no-op.
    if (!shutting_down_.exchange(true, std::memory_order_acq_rel)) {
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }
}

void Server::request_shutdown() noexcept {
    if (!shutting_down_.exchange(true, std::memory_order_acq_rel)) {
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }
}

void Server::run() {
    while (!shutting_down_.load(std::memory_order_acquire)) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int client_fd;
        do {
            client_fd = ::accept(listen_fd_,
                                 reinterpret_cast<sockaddr*>(&peer),
                                 &peer_len);
        } while (client_fd < 0 && errno == EINTR);

        if (client_fd < 0) {
            if (shutting_down_.load(std::memory_order_acquire)) break;
            if (errno == EMFILE || errno == ENFILE) {
                // Out of file descriptors; back off briefly so we don't burn
                // CPU in a tight loop while the situation clears.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // Unexpected accept error: best-effort continue. A production
            // server would log; we keep stdout/stderr clean for the demo.
            continue;
        }

        // Disable Nagle so single-request latencies aren't bunched into
        // 40 ms send batches — matters mostly for the benchmark.
        int one = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        // Increment BEFORE submit so the await loop below cannot observe a
        // false "drained" state between submit and the handler's increment.
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        try {
            pool_.submit([this, client_fd] { handle_connection(client_fd); });
        } catch (...) {
            // Pool refused (shutdown). Roll back the count and bail.
            ::close(client_fd);
            if (in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(in_flight_mtx_);
                in_flight_cv_.notify_all();
            }
            break;
        }
    }

    // Wait until every handler we dispatched has finished accessing `this`.
    std::unique_lock lock(in_flight_mtx_);
    in_flight_cv_.wait(lock, [this] {
        return in_flight_.load(std::memory_order_acquire) == 0;
    });
}

void Server::handle_connection(int client_fd) {
    std::string buf;
    try {
        while (!shutting_down_.load(std::memory_order_relaxed)) {
            auto line = read_line(client_fd, buf, kMaxLineBytes);
            if (!line) break;

            auto parsed = parse_line(*line);
            std::string reply = std::visit([&](auto&& arg) -> std::string {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, Command>) {
                    return dispatch(store_, arg);
                } else {
                    return respond_error(arg.message);
                }
            }, parsed);

            if (!write_all(client_fd, reply)) break;
        }
    } catch (const std::exception& e) {
        // Per-connection boundary. The only realistic exceptions here are
        // std::bad_alloc from a huge value or keys() result. Convert to an
        // ERROR response and close instead of letting the exception escape
        // into the pool worker (which would unwind through worker_loop's
        // catch-all and silently swallow the failure).
        write_all(client_fd, respond_error(std::string("internal: ") + e.what()));
    } catch (...) {
        write_all(client_fd, respond_error("internal: unknown"));
    }

    ::close(client_fd);

    if (in_flight_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lock(in_flight_mtx_);
        in_flight_cv_.notify_all();
    }
}
