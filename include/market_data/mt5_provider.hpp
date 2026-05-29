#pragma once

#include <chrono>
#include <mutex>
#include <string>

#include "provider.hpp"

namespace market_data {

// TCP NDJSON client for the Python MT5 sidecar.
//
// Connection lifecycle:
//   - Lazy connect on first use.
//   - On any I/O or protocol error: close socket, schedule reconnect with
//     exponential backoff (1s -> 30s cap). Failed call returns Quote{-1}.
//   - Successful response resets backoff to 1s.
//
// Thread-safety: a mutex serializes socket access. The engine cycle is
// single-threaded today, but the lock means future parallelization can't
// silently interleave NDJSON requests on the same socket.
class MT5Provider : public IMarketDataProvider {
public:
    MT5Provider(std::string host,
                int port,
                int connect_timeout_ms = 2000,
                int io_timeout_ms      = 3000);

    ~MT5Provider() override;

    MT5Provider(const MT5Provider&) = delete;
    MT5Provider& operator=(const MT5Provider&) = delete;

    Quote fetchQuote(const std::string& symbol) override;

    // Heartbeat. Returns true iff the bridge replied with ok=true.
    // Used by the engine on (re)connect to confirm demo_only state.
    bool ping(std::string* server_account_label = nullptr,
              bool*        demo_only            = nullptr);

private:
    bool        ensureConnected();
    void        closeSocket();
    bool        sendLine(const std::string& line);
    bool        recvLine(std::string& out);
    std::string requestResponse(const std::string& payload);

    std::string host_;
    int         port_;
    int         connect_timeout_ms_;
    int         io_timeout_ms_;

    int         sock_         = -1;
    int         backoff_secs_ = 1;
    std::chrono::steady_clock::time_point next_attempt_;
    std::mutex  mu_;
};

}
