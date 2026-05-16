#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

// Wire protocol (line-delimited ASCII, "\n" or "\r\n" terminator):
//
//   Requests           Responses
//   ----------------   --------------------
//   SET k v ...        OK
//   GET k              VALUE <v>   |  NOT_FOUND
//   DEL k              OK          |  NOT_FOUND
//   EXISTS k           OK          |  NOT_FOUND
//   EXPIRE k <secs>    OK          |  NOT_FOUND
//   KEYS               COUNT <n>\n KEY <k1>\n KEY <k2>\n ...
//
// SET takes the rest of the line after the key as the value, so values may
// contain spaces but not newlines. Keys may not contain any whitespace.
// Anything that fails to parse yields ERROR <message>.

enum class Verb { SET, GET, DEL, EXISTS, KEYS, EXPIRE };

struct Command {
    Verb verb;
    std::string key;                 // empty for KEYS
    std::string value;               // SET only
    std::chrono::seconds ttl{0};     // EXPIRE / set_with_ttl only
};

struct ParseError {
    std::string message;
};

using ParseResult = std::variant<Command, ParseError>;

// `line` is one logical request line with the trailing newline removed. A
// trailing '\r' is tolerated (CRLF clients).
ParseResult parse_line(std::string_view line);

std::string respond_ok();
std::string respond_value(std::string_view v);
std::string respond_not_found();
std::string respond_count(std::size_t n);
std::string respond_key(std::string_view k);
std::string respond_error(std::string_view msg);

// Soft cap on a single request line. The server enforces this on the read
// path; parse_line also rejects oversized input so the parser can be exercised
// in isolation.
inline constexpr std::size_t kMaxLineBytes = std::size_t{1} << 20;   // 1 MiB
