// Pre-Component-5 scaffolding: exercises Store and Protocol in isolation.
// Replaced by the real server wire-up in Component 5.

#include "protocol.hpp"
#include "store.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <variant>
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

bool is_ok(const ParseResult& r) {
    return std::holds_alternative<Command>(r);
}
bool is_err(const ParseResult& r) {
    return std::holds_alternative<ParseError>(r);
}
const Command& cmd(const ParseResult& r) {
    return std::get<Command>(r);
}

void test_protocol_parse() {
    using namespace std::chrono_literals;

    // KEYS
    {
        auto r = parse_line("KEYS");
        assert(is_ok(r) && cmd(r).verb == Verb::KEYS);
    }
    assert(is_err(parse_line("KEYS extra")));

    // GET / DEL / EXISTS — single key, no whitespace
    {
        auto r = parse_line("GET hello");
        assert(is_ok(r) && cmd(r).verb == Verb::GET && cmd(r).key == "hello");
    }
    {
        auto r = parse_line("GET hello\r");          // CRLF tolerated
        assert(is_ok(r) && cmd(r).key == "hello");
    }
    assert(is_err(parse_line("GET")));
    assert(is_err(parse_line("GET ")));              // trailing space, empty key
    assert(is_err(parse_line("GET k extra")));
    assert(is_err(parse_line("DEL")));
    assert(is_ok(parse_line("DEL k")));
    assert(is_ok(parse_line("EXISTS k")));

    // SET — rest-of-line value
    {
        auto r = parse_line("SET k v");
        assert(is_ok(r) && cmd(r).verb == Verb::SET);
        assert(cmd(r).key == "k" && cmd(r).value == "v");
    }
    {
        auto r = parse_line("SET k hello world");
        assert(is_ok(r));
        assert(cmd(r).key == "k" && cmd(r).value == "hello world");
    }
    {
        auto r = parse_line("SET k hello\tworld");    // tab is part of value
        assert(is_ok(r) && cmd(r).value == "hello\tworld");
    }
    assert(is_err(parse_line("SET")));
    assert(is_err(parse_line("SET k")));              // missing value
    assert(is_err(parse_line("SET k ")));             // empty value
    assert(is_err(parse_line("SET  k v")));           // double space => empty key

    // EXPIRE
    {
        auto r = parse_line("EXPIRE k 5");
        assert(is_ok(r) && cmd(r).verb == Verb::EXPIRE);
        assert(cmd(r).key == "k" && cmd(r).ttl == 5s);
    }
    {
        auto r = parse_line("EXPIRE k 0");
        assert(is_ok(r) && cmd(r).ttl == 0s);
    }
    assert(is_err(parse_line("EXPIRE k -1")));
    assert(is_err(parse_line("EXPIRE k abc")));
    assert(is_err(parse_line("EXPIRE k 5 extra")));   // trailing garbage
    assert(is_err(parse_line("EXPIRE k")));           // missing ttl
    assert(is_err(parse_line("EXPIRE")));

    // Unknown / empty
    assert(is_err(parse_line("HELLO")));
    assert(is_err(parse_line("")));

    // Oversize line
    {
        std::string huge(kMaxLineBytes + 1, 'x');
        assert(is_err(parse_line(huge)));
    }

    std::cout << "[ok] protocol parser\n";
}

void test_protocol_responses() {
    assert(respond_ok() == "OK\n");
    assert(respond_not_found() == "NOT_FOUND\n");
    assert(respond_value("x") == "VALUE x\n");
    assert(respond_value("hello world") == "VALUE hello world\n");
    assert(respond_count(0) == "COUNT 0\n");
    assert(respond_count(42) == "COUNT 42\n");
    assert(respond_key("foo") == "KEY foo\n");
    assert(respond_error("oops") == "ERROR oops\n");
    std::cout << "[ok] protocol response builders\n";
}

}  // namespace

int main() {
    test_single_threaded();
    test_ttl();
    test_concurrency();
    test_protocol_parse();
    test_protocol_responses();
    std::cout << "Store + Protocol: all tests passed\n";
    return 0;
}
