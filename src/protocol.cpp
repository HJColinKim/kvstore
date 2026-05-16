#include "protocol.hpp"

#include <charconv>
#include <system_error>

namespace {

bool contains_whitespace(std::string_view s) noexcept {
    return s.find_first_of(" \t\r\n") != std::string_view::npos;
}

ParseResult parse_no_args(Verb v, std::string_view rest) {
    if (!rest.empty()) return ParseError{"command takes no arguments"};
    return Command{v, {}, {}, {}};
}

ParseResult parse_one_key(Verb v, std::string_view rest) {
    if (rest.empty()) return ParseError{"missing key"};
    if (contains_whitespace(rest)) {
        return ParseError{"key must not contain whitespace"};
    }
    return Command{v, std::string{rest}, {}, {}};
}

ParseResult parse_set(std::string_view rest) {
    // Split exactly once on the first space: everything before is the key,
    // everything after (verbatim) is the value. This is what gives SET its
    // "rest of line" semantics for values with spaces.
    const auto space = rest.find(' ');
    if (space == std::string_view::npos) return ParseError{"missing value"};
    const auto key = rest.substr(0, space);
    const auto value = rest.substr(space + 1);
    if (key.empty()) return ParseError{"empty key"};
    if (value.empty()) return ParseError{"empty value"};
    if (contains_whitespace(key)) {
        return ParseError{"key must not contain whitespace"};
    }
    return Command{Verb::SET, std::string{key}, std::string{value}, {}};
}

ParseResult parse_expire(std::string_view rest) {
    const auto space = rest.find(' ');
    if (space == std::string_view::npos) return ParseError{"missing ttl"};
    const auto key = rest.substr(0, space);
    const auto ttl_token = rest.substr(space + 1);
    if (key.empty()) return ParseError{"empty key"};
    if (ttl_token.empty()) return ParseError{"missing ttl"};
    if (contains_whitespace(key)) {
        return ParseError{"key must not contain whitespace"};
    }

    long long seconds = 0;
    const char* const begin = ttl_token.data();
    const char* const end = begin + ttl_token.size();
    // from_chars requires the entire token to be consumed; trailing garbage
    // (including whitespace) leaves ptr != end and is rejected here.
    const auto [ptr, ec] = std::from_chars(begin, end, seconds);
    if (ec != std::errc{} || ptr != end) return ParseError{"invalid ttl"};
    if (seconds < 0) return ParseError{"ttl must be non-negative"};

    return Command{Verb::EXPIRE, std::string{key}, {}, std::chrono::seconds{seconds}};
}

}  // namespace

ParseResult parse_line(std::string_view line) {
    if (line.size() > kMaxLineBytes) return ParseError{"line too long"};
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) return ParseError{"empty command"};

    const auto space = line.find(' ');
    const auto verb_token = line.substr(0, space);
    const auto rest = (space == std::string_view::npos)
                        ? std::string_view{}
                        : line.substr(space + 1);

    if (verb_token == "GET")    return parse_one_key(Verb::GET, rest);
    if (verb_token == "DEL")    return parse_one_key(Verb::DEL, rest);
    if (verb_token == "EXISTS") return parse_one_key(Verb::EXISTS, rest);
    if (verb_token == "KEYS")   return parse_no_args(Verb::KEYS, rest);
    if (verb_token == "SET")    return parse_set(rest);
    if (verb_token == "EXPIRE") return parse_expire(rest);
    return ParseError{"unknown command"};
}

std::string respond_ok() {
    return "OK\n";
}

std::string respond_value(std::string_view v) {
    std::string out;
    out.reserve(7 + v.size());
    out.append("VALUE ").append(v).append("\n");
    return out;
}

std::string respond_not_found() {
    return "NOT_FOUND\n";
}

std::string respond_count(std::size_t n) {
    return "COUNT " + std::to_string(n) + "\n";
}

std::string respond_key(std::string_view k) {
    std::string out;
    out.reserve(5 + k.size());
    out.append("KEY ").append(k).append("\n");
    return out;
}

std::string respond_error(std::string_view msg) {
    std::string out;
    out.reserve(7 + msg.size());
    out.append("ERROR ").append(msg).append("\n");
    return out;
}
