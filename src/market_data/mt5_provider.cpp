#include "market_data/mt5_provider.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace market_data {

namespace {
constexpr int kMaxBackoffSeconds = 30;
constexpr size_t kMaxLineBytes   = 65536;
}

MT5Provider::MT5Provider(std::string host,
                         int port,
                         int connect_timeout_ms,
                         int io_timeout_ms)
    : host_(std::move(host)),
      port_(port),
      connect_timeout_ms_(connect_timeout_ms),
      io_timeout_ms_(io_timeout_ms),
      next_attempt_(std::chrono::steady_clock::now()) {}

MT5Provider::~MT5Provider() {
    closeSocket();
}

void MT5Provider::closeSocket() {
    if(sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool MT5Provider::ensureConnected() {

    if(sock_ >= 0) {
        return true;
    }

    auto now = std::chrono::steady_clock::now();
    if(now < next_attempt_) {
        return false;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        spdlog::warn("mt5: socket() failed: {}", std::strerror(errno));
        return false;
    }

    // Non-blocking connect so we can honor connect_timeout_ms_ without
    // relying on the OS default (often 60-120s).
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port_));
    if(::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
        spdlog::warn("mt5: inet_pton failed for host {}", host_);
        ::close(fd);
        next_attempt_ = now + std::chrono::seconds(backoff_secs_);
        backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
        return false;
    }

    int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if(rc < 0 && errno != EINPROGRESS) {
        spdlog::warn("mt5: connect to {}:{} failed: {}",
                     host_, port_, std::strerror(errno));
        ::close(fd);
        next_attempt_ = now + std::chrono::seconds(backoff_secs_);
        backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
        return false;
    }

    if(rc < 0) {
        // Wait for writability up to connect_timeout_ms_.
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{ connect_timeout_ms_ / 1000,
                    (connect_timeout_ms_ % 1000) * 1000 };
        rc = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if(rc <= 0) {
            spdlog::warn("mt5: connect to {}:{} timed out", host_, port_);
            ::close(fd);
            next_attempt_ = now + std::chrono::seconds(backoff_secs_);
            backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
            return false;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if(err != 0) {
            spdlog::warn("mt5: connect to {}:{} failed (SO_ERROR={})",
                         host_, port_, std::strerror(err));
            ::close(fd);
            next_attempt_ = now + std::chrono::seconds(backoff_secs_);
            backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
            return false;
        }
    }

    // Back to blocking + per-I/O timeout.
    ::fcntl(fd, F_SETFL, flags);

    timeval iotv{ io_timeout_ms_ / 1000,
                  (io_timeout_ms_ % 1000) * 1000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &iotv, sizeof(iotv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &iotv, sizeof(iotv));

    // Many small request/response lines — Nagle would just add latency.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sock_         = fd;
    backoff_secs_ = 1;
    spdlog::info("mt5: connected to {}:{}", host_, port_);
    return true;
}

bool MT5Provider::sendLine(const std::string& line) {

    size_t off = 0;
    while(off < line.size()) {
        ssize_t n = ::send(sock_, line.data() + off, line.size() - off, MSG_NOSIGNAL);
        if(n <= 0) {
            spdlog::warn("mt5: send failed: {}", std::strerror(errno));
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool MT5Provider::recvLine(std::string& out) {

    out.clear();
    out.reserve(256);

    char buf[1024];
    while(out.size() < kMaxLineBytes) {
        ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
        if(n < 0) {
            spdlog::warn("mt5: recv failed: {}", std::strerror(errno));
            return false;
        }
        if(n == 0) {
            spdlog::warn("mt5: peer closed connection");
            return false;
        }
        for(ssize_t i = 0; i < n; ++i) {
            if(buf[i] == '\n') {
                out.append(buf, i);
                // Anything past '\n' would belong to a future reply — but the
                // protocol is strictly one-request-one-response synchronous,
                // so this should never happen. If it does, the next recv will
                // see a stale fragment; log and reset to fail safely.
                if(i + 1 < n) {
                    spdlog::warn("mt5: unexpected trailing bytes after newline");
                    return false;
                }
                return true;
            }
        }
        out.append(buf, n);
    }
    spdlog::warn("mt5: line exceeded {} bytes", kMaxLineBytes);
    return false;
}

std::string MT5Provider::requestResponse(const std::string& payload) {

    std::lock_guard<std::mutex> g(mu_);

    if(!ensureConnected()) {
        return {};
    }

    if(!sendLine(payload)) {
        closeSocket();
        next_attempt_ = std::chrono::steady_clock::now() +
                        std::chrono::seconds(backoff_secs_);
        backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
        return {};
    }

    std::string line;
    if(!recvLine(line)) {
        closeSocket();
        next_attempt_ = std::chrono::steady_clock::now() +
                        std::chrono::seconds(backoff_secs_);
        backoff_secs_ = std::min(backoff_secs_ * 2, kMaxBackoffSeconds);
        return {};
    }

    backoff_secs_ = 1;
    return line;
}

Quote MT5Provider::fetchQuote(const std::string& symbol) {

    nlohmann::json req = { {"op", "quote"}, {"symbol", symbol} };
    std::string raw = requestResponse(req.dump() + "\n");
    if(raw.empty()) {
        return Quote{-1.0};
    }

    try {
        auto j = nlohmann::json::parse(raw);
        if(!j.value("ok", false)) {
            spdlog::warn("mt5: quote {} rejected: {}",
                         symbol, j.value("error", "<no error>"));
            return Quote{-1.0};
        }
        return Quote{ j.at("price").get<double>() };
    }
    catch(const std::exception& e) {
        spdlog::warn("mt5: quote {} parse failure: {} (raw='{}')",
                     symbol, e.what(), raw);
        // A parse failure may indicate desync — close so we resync on next call.
        std::lock_guard<std::mutex> g(mu_);
        closeSocket();
        return Quote{-1.0};
    }
}

bool MT5Provider::ping(std::string* server_account_label, bool* demo_only) {

    nlohmann::json req = { {"op", "ping"} };
    std::string raw = requestResponse(req.dump() + "\n");
    if(raw.empty()) {
        return false;
    }
    try {
        auto j = nlohmann::json::parse(raw);
        if(!j.value("ok", false)) {
            return false;
        }
        if(demo_only) {
            *demo_only = j.value("demo_only", true);
        }
        if(server_account_label) {
            auto acct = j.value("account", nlohmann::json::object());
            std::string login  = std::to_string(acct.value("login", 0));
            std::string server = acct.value("server", std::string{});
            std::string mode   = acct.value("mode",   std::string{});
            *server_account_label = "#" + login + "@" + server + " (" + mode + ")";
        }
        return true;
    }
    catch(const std::exception& e) {
        spdlog::warn("mt5: ping parse failure: {} (raw='{}')", e.what(), raw);
        std::lock_guard<std::mutex> g(mu_);
        closeSocket();
        return false;
    }
}

}
