// Multi-threaded throughput / latency benchmark for kvstore-server. Each
// worker opens its own TCP connection, all workers cross a std::latch gate
// together so the wall-clock timing is fair, and per-thread latency samples
// are merged at the end for percentile reporting.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <latch>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

struct Config {
    std::string   host        = "127.0.0.1";
    std::uint16_t port        = 6379;
    std::size_t   threads     = 4;
    std::size_t   ops         = 100'000;     // total ops across all threads
    double        read_ratio  = 0.5;
    std::size_t   keyspace    = 1000;
    std::size_t   value_size  = 16;
};

struct ThreadResult {
    std::vector<std::uint64_t> latencies_ns;
    std::size_t errors = 0;
};

// --- shared TCP plumbing (duplicated from client.cpp on purpose: keeping the
// benchmark a single self-contained source file is more valuable here than
// a one-line shared header). -------------------------------------------------

int connect_to(const char* host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    const int gai = ::getaddrinfo(host, port_str.c_str(), &hints, &res);
    if (gai != 0) {
        throw std::runtime_error(std::string("getaddrinfo: ") + ::gai_strerror(gai));
    }

    int fd = -1;
    int last_errno = 0;
    for (auto* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) { last_errno = errno; continue; }
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        last_errno = errno;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) throw std::system_error(last_errno, std::generic_category(), "connect");

    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n;
        do {
            n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

std::optional<std::string> read_line(int fd, std::string& buf) {
    while (true) {
        if (auto nl = buf.find('\n'); nl != std::string::npos) {
            std::string line(buf, 0, nl);
            buf.erase(0, nl + 1);
            return line;
        }
        char chunk[4096];
        ssize_t n;
        do {
            n = ::recv(fd, chunk, sizeof(chunk), 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) return std::nullopt;
        buf.append(chunk, static_cast<std::size_t>(n));
    }
}

// --- worker -----------------------------------------------------------------

void worker(const Config& cfg, std::size_t thread_idx,
            std::latch& start_gate, ThreadResult& result) {
    const std::size_t ops_per_thread = cfg.ops / cfg.threads;
    const std::size_t warmup = ops_per_thread / 10;
    result.latencies_ns.reserve(ops_per_thread);

    int fd = -1;
    try {
        fd = connect_to(cfg.host.c_str(), cfg.port);
    } catch (const std::exception&) {
        ++result.errors;
    }

    // Always cross the gate — otherwise a failed connect deadlocks main.
    start_gate.arrive_and_wait();
    if (fd < 0) return;

    std::mt19937 rng(static_cast<unsigned>(thread_idx) + 1u);
    std::uniform_int_distribution<std::size_t> key_dist(0, cfg.keyspace - 1);
    std::uniform_real_distribution<double> op_dist(0.0, 1.0);

    const std::string value(cfg.value_size, 'x');
    std::string req;
    req.reserve(64 + cfg.value_size);
    std::string recv_buf;

    for (std::size_t i = 0; i < warmup + ops_per_thread; ++i) {
        const auto key = key_dist(rng);
        const bool is_read = op_dist(rng) < cfg.read_ratio;

        // Build the request OUTSIDE the timed window so we measure server
        // round-trip, not std::to_string + string concatenation overhead.
        req.clear();
        if (is_read) {
            req.append("GET k").append(std::to_string(key)).push_back('\n');
        } else {
            req.append("SET k").append(std::to_string(key)).push_back(' ');
            req.append(value).push_back('\n');
        }

        const auto t0 = std::chrono::steady_clock::now();
        if (!write_all(fd, req)) { ++result.errors; break; }
        auto line = read_line(fd, recv_buf);
        const auto t1 = std::chrono::steady_clock::now();
        if (!line) { ++result.errors; break; }

        if (i >= warmup) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            result.latencies_ns.push_back(static_cast<std::uint64_t>(ns));
        }
    }

    ::close(fd);
}

// --- arg parsing ------------------------------------------------------------

template <typename T>
T parse_int(std::string_view arg, const char* name) {
    T out{};
    const auto* begin = arg.data();
    const auto* end = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument(std::string("invalid ") + name + ": '" + std::string(arg) + "'");
    }
    return out;
}

double parse_double(std::string_view arg, const char* name) {
    // std::from_chars for double is in libstdc++ 11+; covered everywhere we run.
    double out = 0.0;
    const auto* begin = arg.data();
    const auto* end = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(begin, end, out);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument(std::string("invalid ") + name + ": '" + std::string(arg) + "'");
    }
    return out;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--host H] [--port P] [--threads N] [--ops M]"
              << " [--read-ratio R] [--keyspace K] [--value-size B]\n";
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a{argv[i]};
        auto need_value = [&](const char* name) -> std::string_view {
            if (i + 1 >= argc) {
                throw std::invalid_argument(std::string(name) + " requires a value");
            }
            return std::string_view{argv[++i]};
        };
        if      (a == "--host")        cfg.host = std::string(need_value("--host"));
        else if (a == "--port")        cfg.port = parse_int<std::uint16_t>(need_value("--port"), "port");
        else if (a == "--threads")     cfg.threads = parse_int<std::size_t>(need_value("--threads"), "threads");
        else if (a == "--ops")         cfg.ops = parse_int<std::size_t>(need_value("--ops"), "ops");
        else if (a == "--read-ratio")  cfg.read_ratio = parse_double(need_value("--read-ratio"), "read-ratio");
        else if (a == "--keyspace")    cfg.keyspace = parse_int<std::size_t>(need_value("--keyspace"), "keyspace");
        else if (a == "--value-size")  cfg.value_size = parse_int<std::size_t>(need_value("--value-size"), "value-size");
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); std::exit(0); }
        else throw std::invalid_argument(std::string("unknown arg: ") + std::string(a));
    }

    if (cfg.threads == 0) cfg.threads = 1;
    if (cfg.keyspace == 0) cfg.keyspace = 1;
    if (cfg.ops < cfg.threads) cfg.ops = cfg.threads;
    if (cfg.read_ratio < 0.0 || cfg.read_ratio > 1.0) {
        throw std::invalid_argument("--read-ratio must be in [0, 1]");
    }
    return cfg;
}

// --- stats ------------------------------------------------------------------

double percentile_us(const std::vector<std::uint64_t>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const std::size_t idx = std::min(sorted.size() - 1,
        static_cast<std::size_t>(p * sorted.size()));
    return static_cast<double>(sorted[idx]) / 1000.0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Config cfg = parse_args(argc, argv);

        std::cout << std::format(
            "kvstore-bench\n"
            "  host={}:{} threads={} ops={} read-ratio={:.2f} keyspace={} value-size={}\n",
            cfg.host, cfg.port, cfg.threads, cfg.ops,
            cfg.read_ratio, cfg.keyspace, cfg.value_size);

        std::vector<ThreadResult> results(cfg.threads);
        std::latch start_gate(static_cast<std::ptrdiff_t>(cfg.threads + 1));

        std::vector<std::jthread> workers;
        workers.reserve(cfg.threads);
        for (std::size_t t = 0; t < cfg.threads; ++t) {
            workers.emplace_back([&, t] {
                worker(cfg, t, start_gate, results[t]);
            });
        }

        start_gate.arrive_and_wait();
        const auto wall_start = std::chrono::steady_clock::now();
        workers.clear();   // joins every jthread
        const auto wall_end = std::chrono::steady_clock::now();

        // Merge and sort all latency samples.
        std::vector<std::uint64_t> all;
        std::size_t total_errors = 0;
        std::size_t total_samples = 0;
        for (const auto& r : results) {
            total_errors += r.errors;
            total_samples += r.latencies_ns.size();
        }
        all.reserve(total_samples);
        for (auto& r : results) {
            all.insert(all.end(), r.latencies_ns.begin(), r.latencies_ns.end());
        }
        std::sort(all.begin(), all.end());

        const double seconds =
            std::chrono::duration<double>(wall_end - wall_start).count();
        const double ops_per_sec = (seconds > 0.0)
            ? static_cast<double>(all.size()) / seconds : 0.0;

        const double mean_us = all.empty() ? 0.0
            : (static_cast<double>(std::accumulate(all.begin(), all.end(), std::uint64_t{0}))
               / static_cast<double>(all.size())) / 1000.0;

        std::cout << std::format(
            "  measured     : {} ops total ({} warmup discarded per thread)\n"
            "  wall time    : {:.3f} s\n"
            "  throughput   : {:.0f} ops/sec\n"
            "  latency (us) : mean {:.1f}  p50 {:.1f}  p90 {:.1f}  p99 {:.1f}  p99.9 {:.1f}  max {:.1f}\n"
            "  errors       : {}\n",
            all.size(), (cfg.ops / cfg.threads) / 10,
            seconds, ops_per_sec,
            mean_us,
            percentile_us(all, 0.50),
            percentile_us(all, 0.90),
            percentile_us(all, 0.99),
            percentile_us(all, 0.999),
            all.empty() ? 0.0 : static_cast<double>(all.back()) / 1000.0,
            total_errors);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
