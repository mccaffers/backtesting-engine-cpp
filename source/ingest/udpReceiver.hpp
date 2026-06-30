// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

// A minimal UDP datagram receiver. The Boost.Asio implementation is hidden
// behind a pimpl so this header stays free of Asio's heavy templates — that
// keeps it safe to #include from a C++23 module's global module fragment
// (ingestCommand.cppm), the same isolation the repo uses for Boost.Redis
// (boostRedisImpl.cpp) and the Redis connection (redisConnection.hpp).
namespace ingest {

class UdpReceiver {
public:
    // Invoked once per datagram with a view over the received bytes. The span is
    // valid only for the duration of the call (it aliases an internal buffer).
    // Called on the run() thread.
    using Handler = std::function<void(std::span<const std::byte>)>;

    UdpReceiver(std::string bindAddr, std::uint16_t port, Handler onDatagram);
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&) = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Bind the socket and run the receive loop. Blocks until stop() is called or
    // SIGINT/SIGTERM is received (the receiver installs its own signal handler
    // for clean shutdown). Returns false without blocking if the socket cannot be
    // bound (bad address / port already in use) — the cause is logged. Returns
    // true after a clean shutdown.
    [[nodiscard]] bool run();

    // Unblock run() from another thread (or a signal). Idempotent.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ingest
