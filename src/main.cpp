// Component 2 scaffolding: exercises the Store API. Replaced by the real
// server wire-up in Component 5.

#include "store.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

void test_single_threaded() {
    Store s(4);
    s.set("a", "1");
    assert(s.exists("a"));
    assert(s.get("a") == std::optional<std::string>{"1"});

    s.set("a", "2");
    assert(s.get("a") == std::optional<std::string>{"2"});

    assert(s.del("a"));
    assert(!s.exists("a"));
    assert(!s.del("a"));
    assert(s.get("a") == std::nullopt);

    std::cout << "[ok] single-threaded set/get/del/exists\n";
}

void test_ttl() {
    using namespace std::chrono_literals;
    Store s(4);

    s.set_with_ttl("k", "v", 1s);
    assert(s.exists("k"));
    std::this_thread::sleep_for(1100ms);
    assert(!s.exists("k"));
    assert(s.get("k") == std::nullopt);

    s.set("k2", "v2");
    assert(s.expire("k2", 0s));      // deadline = now -> immediately expired
    assert(!s.exists("k2"));         // lazy sweep on the read path
    assert(!s.expire("missing", 5s));

    std::cout << "[ok] TTL expiry (lazy)\n";
}

void test_concurrency() {
    constexpr int threads = 8;
    constexpr int ops_per_thread = 100'000;
    constexpr int keyspace = 1000;

    Store s(16);
    std::vector<std::jthread> workers;
    workers.reserve(threads);

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(static_cast<unsigned>(t) + 1);
            std::uniform_int_distribution<int> key_dist(0, keyspace - 1);
            std::uniform_int_distribution<int> op_dist(0, 9);
            for (int i = 0; i < ops_per_thread; ++i) {
                const auto key = std::to_string(key_dist(rng));
                const int op = op_dist(rng);
                if (op < 5)        s.set(key, "v" + std::to_string(i));
                else if (op < 9)   (void)s.get(key);
                else               (void)s.del(key);
            }
        });
    }
    workers.clear();   // joins each jthread in its destructor

    const auto live = s.keys();
    assert(live.size() <= static_cast<std::size_t>(keyspace));

    std::cout << "[ok] concurrency smoke (" << threads << " threads, "
              << ops_per_thread << " ops/thread, " << live.size()
              << " keys live)\n";
}

}  // namespace

int main() {
    test_single_threaded();
    test_ttl();
    test_concurrency();
    std::cout << "Store: all tests passed\n";
    return 0;
}
