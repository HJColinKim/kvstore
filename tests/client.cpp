// Minimal interactive CLI for kvstore-server. Reads stdin line by line,
// forwards each line verbatim to the server, prints the server's reply.
// Handles the one multi-line response shape (COUNT n followed by n KEY lines)
// so KEYS works ergonomically; everything else is a single line.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

int connect_to(const char* host, std::uint16_t port) {
    // getaddrinfo for portability across IPv4/IPv6 and hostname resolution;
    // beats inet_pton which only handles numeric addresses.
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
    if (fd < 0) {
        throw std::system_error(last_errno, std::generic_category(), "connect");
    }
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

// One '\n'-terminated line read from `fd`, accumulating across calls in `buf`.
// Returns nullopt on EOF or error.
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

// Reads one server response and prints it. Single-line for most replies; for
// "COUNT N", also reads and prints N follow-up KEY lines.
bool read_response(int fd, std::string& buf) {
    const auto first = read_line(fd, buf);
    if (!first) return false;
    std::cout << *first << '\n';

    constexpr std::string_view count_prefix = "COUNT ";
    if (first->compare(0, count_prefix.size(), count_prefix) != 0) return true;

    const std::string_view num_sv{*first};
    const auto* begin = num_sv.data() + count_prefix.size();
    const auto* end = num_sv.data() + num_sv.size();
    std::size_t n = 0;
    auto [ptr, ec] = std::from_chars(begin, end, n);
    if (ec != std::errc{} || ptr != end) {
        std::cerr << "client: malformed COUNT response\n";
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        auto line = read_line(fd, buf);
        if (!line) return false;
        std::cout << *line << '\n';
    }
    return true;
}

std::uint16_t parse_port(std::string_view arg) {
    std::uint16_t port = 0;
    const auto* begin = arg.data();
    const auto* end = arg.data() + arg.size();
    auto [ptr, ec] = std::from_chars(begin, end, port);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument(std::string("invalid port: ") + std::string(arg));
    }
    return port;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const char* host = (argc > 1) ? argv[1] : "127.0.0.1";
        const std::uint16_t port = (argc > 2) ? parse_port(argv[2]) : 6379;
        if (argc > 3) {
            std::cerr << "Usage: " << argv[0] << " [host [port]]\n";
            return 2;
        }

        const int fd = connect_to(host, port);
        const bool interactive = ::isatty(STDIN_FILENO) != 0;

        std::string recv_buf;
        std::string line;
        while (true) {
            if (interactive) {
                std::cout << "> " << std::flush;
            }
            if (!std::getline(std::cin, line)) break;
            line.push_back('\n');
            if (!write_all(fd, line)) {
                std::cerr << "client: server closed connection\n";
                break;
            }
            if (!read_response(fd, recv_buf)) {
                std::cerr << "client: server closed connection\n";
                break;
            }
        }

        ::close(fd);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
