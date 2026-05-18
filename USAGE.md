# Usage Guide

Step-by-step instructions for building, running, and exercising `kvstore`
end-to-end on a Linux or WSL machine.

## Prerequisites

You need:

- A POSIX environment: **Linux**, or **WSL** (Windows Subsystem for Linux) on
  Windows. Native Windows is not supported — the server uses POSIX socket
  APIs directly.
- `g++` with C++20 support. **gcc 13 or later is recommended** because the
  benchmark uses `<format>`, which only became fully usable in libstdc++ 13.
- `cmake` 3.15 or later.
- `make` (or another CMake-supported backend like `ninja`).
- `git` (to clone the repo).
- `nc` / `netcat` is handy for manual protocol testing but not required.

### One-time setup on WSL Ubuntu

WSL Ubuntu often ships with `gcc` but **not `g++`**, and may be missing
`cmake` and other build essentials. Install everything in one go:

```bash
sudo apt update
sudo apt install -y build-essential cmake
```

Verify:

```bash
g++ --version    # should report 13.x or newer for full std::format support
cmake --version  # should report 3.15+
```

If your distro's default `g++` is older than 13, install a newer one:

```bash
sudo apt install -y g++-13
export CXX=g++-13
```

(or pass `-DCMAKE_CXX_COMPILER=g++-13` to `cmake`).

### Where to keep the source

If you're on WSL and the repo currently lives on the Windows side
(e.g., `C:\Users\you\Downloads\kvstore`), it is accessible from WSL as
`/mnt/c/Users/you/Downloads/kvstore`. **Build performance on `/mnt/c` is
substantially slower** than on the WSL native filesystem. For development,
clone to `~/kvstore` (or anywhere under `~`) and work there.

## Building

From the repository root:

```bash
cmake -S . -B build
cmake --build build -j
```

This produces three executables under `build/`:

| Binary             | Purpose                                       |
|--------------------|-----------------------------------------------|
| `kvstore-server`   | The TCP server                                |
| `kvstore-cli`      | Interactive command-line client               |
| `kvstore-bench`    | Multi-threaded throughput / latency benchmark |

The build is warning-clean under `-Wall -Wextra -Wpedantic -O2`. If you
see warnings, that's a regression; please report it.

### Debug build (optional)

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

The default build doesn't set `CMAKE_BUILD_TYPE`; compile options are
applied explicitly so a Release/Debug switch is opt-in.

## Running the server

Default port 6379:

```bash
./build/kvstore-server
```

With explicit configuration (positional, all optional):

```bash
./build/kvstore-server [port [shards [threads]]]
```

| Argument | Meaning                              | Default                        |
|----------|--------------------------------------|--------------------------------|
| `port`   | TCP port to listen on                | `6379`                         |
| `shards` | Number of `Store` shards             | `16`                           |
| `threads`| Worker pool size                     | `std::thread::hardware_concurrency()` |

Example — port 7000, 32 shards, 4 worker threads:

```bash
./build/kvstore-server 7000 32 4
```

Startup output:

```
kvstore-server listening on :7000 shards=32 threads=4
```

### Shutting down

Send `SIGINT` (`Ctrl+C` in the foreground terminal) or `SIGTERM`
(`kill <pid>`):

```
^C
kvstore-server: shutdown complete
```

If clients are still connected and idle, shutdown waits for them to
disconnect (the listening socket closes immediately, but in-flight
connection handlers run to natural EOF). To force-shutdown, disconnect
clients first.

## Interactive use: `kvstore-cli`

```bash
./build/kvstore-cli [host [port]]
```

Defaults to `127.0.0.1:6379`. Type a command per line; the server's
reply is printed verbatim.

Example session:

```
$ ./build/kvstore-cli
> SET foo hello world
OK
> GET foo
VALUE hello world
> EXISTS foo
OK
> EXPIRE foo 5
OK
> KEYS
COUNT 1
KEY foo
> (wait 5 seconds)
> GET foo
NOT_FOUND
> ^D
```

`Ctrl+D` (EOF) terminates the client cleanly.

### Scripted use

Pipe a request stream from stdin:

```bash
printf 'SET k v\nGET k\nDEL k\nGET k\n' | ./build/kvstore-cli
```

Expected output:

```
OK
VALUE v
OK
NOT_FOUND
```

When stdin is not a terminal, the `> ` prompt is suppressed automatically,
so the output is clean for parsing.

## Manual smoke test with `nc`

The protocol is plain text, so any TCP client works. `nc -C` (CRLF mode)
gives you a Telnet-style experience:

```bash
$ nc -C localhost 6379
SET color red
OK
SET shape square
OK
GET color
VALUE red
KEYS
COUNT 2
KEY color
KEY shape
EXPIRE color 0
OK
GET color
NOT_FOUND
INVALID
ERROR unknown command
^D
```

For a thorough smoke test, exercise every verb in one session including
the error paths (malformed commands, missing keys, oversized lines, etc).
The server returns `ERROR <message>` for anything it can't parse or that
fails validation; the connection stays alive on parse errors.

## Running the benchmark

The benchmark connects to a running server and reports throughput and
latency percentiles.

```bash
./build/kvstore-bench [flags]
```

| Flag               | Default       | Description                                                                  |
|--------------------|---------------|------------------------------------------------------------------------------|
| `--host H`         | `127.0.0.1`   | Server host                                                                  |
| `--port P`         | `6379`        | Server port                                                                  |
| `--threads N`      | `4`           | Concurrent client threads, each on its own TCP connection                    |
| `--ops M`          | `100000`      | Total operations across all threads                                          |
| `--read-ratio R`   | `0.5`         | Fraction of ops that are `GET` (`0.0`–`1.0`); the rest are `SET`             |
| `--keyspace K`     | `1000`        | Random key range `k0..k<K-1>`                                                |
| `--value-size B`   | `16`          | `SET` value length in bytes                                                  |

Example — 8 threads, 1 M ops, 80% reads, 10 000-key keyspace, 64-byte values:

```bash
./build/kvstore-bench --threads 8 --ops 1000000 --read-ratio 0.8 \
                     --keyspace 10000 --value-size 64
```

Sample output (your numbers will differ):

```
kvstore-bench
  host=127.0.0.1:6379 threads=8 ops=1000000 read-ratio=0.80 keyspace=10000 value-size=64
  measured     : 1000000 ops total (12500 warmup discarded per thread)
  wall time    : 4.123 s
  throughput   : 242540 ops/sec
  latency (us) : mean 32.4  p50 26.0  p90 48.0  p99 110.0  p99.9 280.0  max 1850.0
  errors       : 0
```

10% of each thread's operations are run as warm-up and discarded from the
latency samples — this avoids first-touch effects (empty keyspace,
cold TCP buffers) from skewing the percentiles.

### Reproducing the README's results table

For the workloads listed in [README.md § Benchmark Results](README.md#results),
run:

```bash
# Start the server in one terminal:
./build/kvstore-server

# In another terminal, run each workload:
./build/kvstore-bench --threads 1 --ops 100000  --read-ratio 1.0
./build/kvstore-bench --threads 8 --ops 1000000 --read-ratio 1.0
./build/kvstore-bench --threads 8 --ops 1000000 --read-ratio 0.8
./build/kvstore-bench --threads 8 --ops 1000000 --read-ratio 0.5
```

Copy the resulting throughput / p50 / p99 / p99.9 numbers into the README
table.

## Troubleshooting

### `bind: Address already in use`

Another process is using the port, or the previous server is still in
TIME_WAIT. `SO_REUSEADDR` is set, so this normally self-resolves within a
few seconds — wait, or pick a different port (e.g. `./build/kvstore-server 7000`).

### `arpa/inet.h: No such file or directory`

You're building on a non-POSIX environment (native Windows, MSYS2 UCRT64,
MinGW). Build inside WSL or on a Linux machine instead.

### `g++: command not found`

You probably have `gcc` (the C compiler) but not `g++` (the C++ compiler).
Run `sudo apt install g++ build-essential`. Confirm with `g++ --version`.

### `error: 'format' is not a member of 'std'`

Your `libstdc++` is too old. `<format>` requires gcc 13+. Either:

- Install a newer compiler (`sudo apt install g++-13` and re-run cmake
  with `CXX=g++-13 cmake -S . -B build`), or
- Edit `tests/benchmark.cpp` to use `printf`-style output instead of
  `std::format`.

### Server hangs on `Ctrl+C`

You have an idle TCP client still connected. Disconnect it (close the
`nc` session, hit `Ctrl+D` in the CLI, etc.) and shutdown will complete.
This is by design — see [README.md § Limitations](README.md#limitations).

### `EXPIRE` doesn't seem to work

Two things to check:

1. `EXPIRE` returns `NOT_FOUND` if the key doesn't exist or is already
   expired — not an error.
2. TTL is in **integer seconds**. Sub-second TTLs are not supported.

### My IDE flags valid C++20 code (e.g. red squiggle under `.find(key)` in `store.cpp`)

That call uses C++20 heterogeneous lookup (`unordered_map<string,...>::find(string_view)`),
enabled by the `is_transparent` typedefs on `StringHash` / `StringEqual`. Real
builds with `g++` accept it; older language servers may not.

The CMake config emits `build/compile_commands.json` so the IDE picks up the
real compile flags. Point your language server at it:

- **VS Code + clangd**: install the *clangd* extension; it finds the file automatically.
- **VS Code + Microsoft C/C++ extension**: set `"C_Cpp.default.compileCommands": "${workspaceFolder}/build/compile_commands.json"` and reload.
- **Neovim + clangd**: clangd discovers `compile_commands.json` automatically when launched in the project root.

Re-run `cmake -S . -B build` if the file doesn't exist yet.

### Client hangs after sending a request

The server received malformed input and disconnected without responding
(e.g., a single request line longer than 1 MiB). Reconnect and check the
request size.

### CLI can't connect: `connect: Connection refused`

The server isn't running on the port the client is targeting. Verify
with `ss -ltnp | grep 6379` (or whatever port you're using).

## Project verification checklist

After building, run this end-to-end sequence to confirm everything works:

```bash
# 1. Build
cmake -S . -B build && cmake --build build -j

# 2. Server starts cleanly
./build/kvstore-server &
sleep 0.5

# 3. Basic round-trip
printf 'SET k v\nGET k\nDEL k\nGET k\n' | ./build/kvstore-cli
# Expected:
#   OK
#   VALUE v
#   OK
#   NOT_FOUND

# 4. TTL works
printf 'SET k v\nEXPIRE k 1\nEXISTS k\n' | ./build/kvstore-cli
sleep 1.2
printf 'EXISTS k\n' | ./build/kvstore-cli
# Expected:
#   OK
#   OK
#   OK
#   NOT_FOUND

# 5. Concurrent throughput
./build/kvstore-bench --threads 8 --ops 100000 --read-ratio 0.8

# 6. Clean shutdown
kill %1
wait
# Expected:
#   kvstore-server: shutdown complete
```

If every step above produces the expected output, the project is working
as designed.
