# kvstore

A multi-threaded, in-memory key–value store in modern C++20 with a
line-delimited TCP wire protocol. Standard library + POSIX sockets only;
no third-party dependencies.

The codebase is intentionally small (~1.2 KLOC) and self-contained so every
design decision is visible and defensible — sharded `std::shared_mutex`
concurrency, lazy TTL expiry via a documented lock-upgrade pattern, a C++20
`jthread`/`stop_token` thread pool, and a blocking-I/O TCP server with a
clean SIGINT shutdown path.

For build and run instructions see [USAGE.md](USAGE.md).

## Architecture

```
        ┌────────┐        TCP        ┌─────────────────────────────────┐
        │ Client ├──────────────────►│ kvstore-server                  │
        │ (nc,   │                   │                                 │
        │ CLI,   │                   │  accept loop  (main thread)     │
        │ bench) │                   │       │ submit                  │
        └────────┘                   │       ▼                         │
                                     │  ThreadPool   (N jthreads)      │
                                     │       │ handle_connection       │
                                     │       ▼ read → parse → dispatch │
                                     │  Store    (16 shards,           │
                                     │            each shared_mutex)   │
                                     └─────────────────────────────────┘
```

The main thread runs `accept()` in a loop. Each accepted connection is
submitted to the thread pool, which dispatches a long-lived handler that
reads requests line-by-line, parses them into a typed `Command`, executes
against the `Store`, and writes the response back. The `Store` is partitioned
into N independently locked shards; reads to different shards never contend.

## Repository layout

```
include/
  store.hpp          thread-safe in-memory KV store
  thread_pool.hpp    fixed-size pool of std::jthread workers
  protocol.hpp       line-protocol parser and response builders
  server.hpp         POSIX TCP server, accept loop, dispatch

src/
  store.cpp
  thread_pool.cpp
  protocol.cpp
  server.cpp
  main.cpp           kvstore-server entry point

tests/
  client.cpp         kvstore-cli, interactive REPL
  benchmark.cpp      kvstore-bench, latency/throughput benchmark

CMakeLists.txt
README.md
USAGE.md
```

## Quick start

```bash
cmake -S . -B build && cmake --build build -j
./build/kvstore-server          # listens on :6379
./build/kvstore-cli             # in another terminal
```

Full prerequisites, troubleshooting, and end-to-end smoke-test recipes live
in [USAGE.md](USAGE.md).

## Protocol

The wire protocol is line-delimited ASCII. `\n` terminates a request; a
trailing `\r` is tolerated so CRLF clients (`telnet`, `nc -C`) work. The
maximum line length is **1 MiB**; longer lines disconnect the client.

### Requests

| Command | Form | Notes |
|---|---|---|
| `SET`    | `SET <key> <value...>\n`     | Value is everything after the first space following the key, so values may contain spaces (but not newlines). |
| `GET`    | `GET <key>\n`                | |
| `DEL`    | `DEL <key>\n`                | |
| `EXISTS` | `EXISTS <key>\n`             | |
| `EXPIRE` | `EXPIRE <key> <seconds>\n`   | Non-negative integer seconds. `0` expires immediately. |
| `KEYS`   | `KEYS\n`                     | Snapshot of currently-live keys. |

Keys may not contain whitespace.

### Responses

| Reply                              | Sent for                                                        |
|------------------------------------|-----------------------------------------------------------------|
| `OK\n`                             | Successful `SET`, `DEL`, `EXISTS`, `EXPIRE`                     |
| `VALUE <value>\n`                  | `GET` of a present, unexpired key                               |
| `NOT_FOUND\n`                      | `GET` / `DEL` / `EXISTS` / `EXPIRE` of an absent or expired key |
| `COUNT <n>\n` + n × `KEY <key>\n`  | `KEYS`                                                          |
| `ERROR <message>\n`                | Malformed command, invalid argument, oversized line, internal   |

## Benchmark

Each worker thread opens its own TCP connection. A `std::latch(N+1)`,
released by every worker plus `main`, ensures every worker begins its
measurement window at the same wall-clock instant. Per-thread latency
samples are merged and percentile-reported at the end. Request construction
happens outside the timed window, so the reported latency is server
round-trip, not `std::to_string` overhead.

### Methodology

- Single-process server and client communicating over `127.0.0.1` (TCP
  loopback). Measures the protocol + scheduler + kernel TCP path *plus*
  the store. An in-process variant would be faster but less representative.
- 10% of each thread's operations are discarded as warm-up — this populates
  the keyspace and warms TCP buffers before the percentiles are sampled.
- Latencies are wall-clock for one full `send → recv` round-trip per op.

### Example invocation

```bash
./build/kvstore-bench --threads 8 --ops 1000000 --read-ratio 0.8 --keyspace 10000
```

### Results

Hardware: `<CPU>`, `<RAM>`, `<OS>`, `<g++ version>` — fill in after running.

| Workload          | Threads | Ops | Throughput (ops/s) | p50 (µs) | p99 (µs) | p99.9 (µs) |
|-------------------|---------|-----|--------------------|----------|----------|------------|
| 100% GET          | 1       | 100k | TBD               | TBD      | TBD      | TBD        |
| 100% GET          | 8       | 1M   | TBD               | TBD      | TBD      | TBD        |
| 80% GET / 20% SET | 8       | 1M   | TBD               | TBD      | TBD      | TBD        |
| 50% GET / 50% SET | 8       | 1M   | TBD               | TBD      | TBD      | TBD        |

To populate: run the benchmark for each row and replace `TBD` with the
reported values.

## Design decisions

### Sharded `std::shared_mutex` (Store)

A single global mutex would serialize the entire store; a per-key mutex
would explode in memory. Sharding splits the difference: `N` independent
maps, each with its own `shared_mutex`. The key is hashed and routed to
`hash(key) % N`. Two writes to different shards never contend. Sixteen
shards is enough headroom past common CPU counts on a single-box
deployment.

`std::shared_mutex` rather than `std::mutex` lets concurrent readers proceed
under `shared_lock`; only writers take `unique_lock`.

### Lazy TTL with a lock-upgrade dance (Store)

There is no background expiration thread. `get()` and `exists()` take a
`shared_lock` first; if the entry is observed expired, the shared lock is
dropped, a `unique_lock` is acquired, and the entry is **re-checked** before
erasure — a concurrent writer may have refreshed or removed it. This re-check
is essential: `std::shared_mutex` deliberately does not support upgrading a
shared lock to an exclusive one (deadlock-prone in the general case), so the
drop-and-reacquire is the canonical workaround.

### C++20 heterogeneous hashing (Store)

`std::unordered_map<std::string, Entry, StringHash, StringEqual>` with
`is_transparent` on both functors lets `find(std::string_view)` skip the
temporary `std::string` allocation that older C++ versions required. The
implicit `std::string → std::string_view` conversion means a single
`operator()(string_view)` overload covers both stored and search keys.

### `unique_ptr<Shard>` for a non-movable mutex (Store)

`std::shared_mutex` is neither copyable nor movable. `std::vector<Shard>`
would fail to compile because the vector needs to move elements during
reallocation. Wrapping each shard in `unique_ptr` makes the pointer
movable while the mutex stays put.

### `condition_variable_any` + `stop_token` (ThreadPool)

`std::condition_variable_any::wait(lock, stop_token, predicate)` registers
a `stop_callback` so `request_stop()` (called automatically by
`~jthread`) wakes the worker without an explicit `notify_all`. Plain
`std::condition_variable` lacks this overload.

### Member declaration order = destruction safety (ThreadPool)

`workers_` is declared **last** so it is destroyed **first**. Each
`jthread`'s destructor calls `request_stop()` and then `join()`; while a
worker drains its final queued task, it still touches `tasks_`, `mtx_`,
and `cv_`. Those members must outlive the workers, which means declaring
them earlier. Reversing this order is a subtle use-after-free.

### Drain-on-shutdown (ThreadPool)

The `wait(lock, stop_token, predicate)` overload returns `true` while the
predicate (non-empty queue) holds, even after stop has been requested.
Workers therefore finish every queued task before exiting, which keeps
the `submit` contract clean: *if `submit` returns normally, your task
will run.*

### Variant-typed parser (Protocol)

`parse_line` returns `std::variant<Command, ParseError>` rather than
throwing or returning a `Command` with an error-string field. A malformed
request is the common case, not the exceptional one, and the variant forces
the caller to handle both arms.

### Rest-of-line `SET` semantics (Protocol)

`SET k hello world` sets `k` to `"hello world"`. The parser splits exactly
once on the first space after the verb and again on the first space after
the key; everything else is the value verbatim. Exactly-one-space-between-
tokens is the rule, so `SET  k v` (double space) fails with "empty key"
rather than silently dropping whitespace.

### `std::from_chars` for `EXPIRE` (Protocol)

Non-allocating, locale-independent. The `ptr != end` check after parsing
catches trailing garbage like `EXPIRE k 5 extra`. Contrast `std::stoi`
(throws, allocates, locale-aware) and `atoi` (no error reporting).

### Two-layer 1 MiB line cap (Protocol + Server)

`parse_line` rejects oversized input so the parser is testable in
isolation; the server's `read_line` also caps the per-connection buffer
at the same value so we never grow the buffer past 1 MiB regardless of
parser state. Belt-and-braces, each layer independently correct.

### POSIX sockets directly (Server)

No `<asio>`, no `<boost>`. `socket` → `setsockopt(SO_REUSEADDR)` → `bind` →
`listen` → `accept`. `SO_REUSEADDR` allows a quick restart without waiting
for TIME_WAIT. `TCP_NODELAY` on accepted sockets disables Nagle so single-
request latencies aren't bunched into 40 ms batches. `MSG_NOSIGNAL` on
`send` (with `SIG_IGN` on `SIGPIPE` as belt-and-braces) makes a broken
pipe surface as `EPIPE` instead of killing the process.

### SIGINT close-listen-fd shutdown (Server)

The signal handler atomically exchanges a `shutting_down_` flag and
`close()`s the listening fd. `accept()` returns with `EBADF` and the loop
exits. Both `close()` and the atomic exchange are async-signal-safe (the
former by POSIX rule, the latter by being lock-free on every realistic
target).

### In-flight refcount for safe `Server` destruction (Server)

Connection handlers submitted to the pool capture `this`. If `Server` were
destroyed before the pool drained, those handlers would dangle. `Server::run()`
therefore tracks an `in_flight_` counter and blocks on a condition variable
until every dispatched handler has finished, *before* returning. Destruction
order in `main` (`Server` → `ThreadPool` → `Store`) is then trivially safe.

### `std::latch` start gate (Benchmark)

`std::latch(threads + 1)` with every worker and `main` calling
`arrive_and_wait` releases everyone at the same moment. The wall-clock
throughput number is therefore honest — it isn't dominated by however long
`std::jthread` construction took.

## Limitations

- A long-idle TCP client delays `Ctrl+C` shutdown until it disconnects.
  `Ctrl+C` closes the listening socket immediately but lets in-flight
  handlers run to natural client disconnect.
- `std::hash<std::string_view>` is not adversary-resistant. Out of scope
  for a portfolio project; matters only if exposing to untrusted networks.
- Thread-per-connection-on-pool: a worker blocked in `recv` stays blocked
  until the client sends data or disconnects. Inherent to blocking I/O.
- No persistence — the store is in-memory only.
- The unit-test scaffolding used during development was retired when the
  server replaced the test harness in `src/main.cpp`. A proper test
  framework (gtest, Catch2) is future work.

## Future work

- Async I/O via `epoll` / `io_uring` to decouple workers from connections.
- RESP wire protocol for `redis-cli` compatibility.
- Append-only-log persistence with periodic compaction.
- Replication / clustering.
- Lock-free reads via RCU or hazard pointers.
- `signalfd` / self-pipe-trick shutdown for textbook-correct signal handling.
- Per-connection read timeouts so idle clients can't delay shutdown.
- Proper unit-test framework replacing the harness scaffolding from Components 2–4.
- `std::move_only_function` (C++23) for the task type so handlers can capture
  move-only state.
- Streaming `KEYS` reply rather than buffering the full response.

## Build configuration

- C++20 required (`CMAKE_CXX_STANDARD 20`, `CMAKE_CXX_EXTENSIONS OFF`).
- Default optimization: `-O2`. Warnings: `-Wall -Wextra -Wpedantic`.
- One CMake `INTERFACE` library, `kvstore_common`, carries the compile
  contract; each executable inherits include path, thread library, warning
  flags, and language standard from it via one `target_link_libraries`.
- Threads linked through `find_package(Threads REQUIRED)` / `Threads::Threads`.
