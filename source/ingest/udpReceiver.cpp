// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------
//
// Boost.Asio UDP receiver implementation. All Asio includes are confined to this
// translation unit (classic includes only, no `import std`) so their templates
// never reach the module boundary — mirroring how boostRedisImpl.cpp /
// redisConnection.cpp isolate Boost.Asio from the rest of the build.

#include "ingest/udpReceiver.hpp"

#include <array>
#include <csignal>
#include <cstddef>
#include <utility>

#include <boost/asio.hpp>

#include "shared/utilities/backtestLog.hpp"

namespace ingest {

namespace asio = boost::asio;
using asio::ip::udp;

struct UdpReceiver::Impl {
    asio::io_context ioc;
    udp::socket socket;              // opened/bound in open(), not the ctor
    asio::signal_set signals;
    udp::endpoint sender;            // filled in per datagram (source address)
    std::array<std::byte, 2048> buffer{};  // one max-size datagram, reused
    Handler handler;
    std::string bindAddr;
    std::uint16_t port;

    Impl(std::string addr, std::uint16_t p, Handler h)
        : socket(ioc),
          signals(ioc, SIGINT, SIGTERM),
          handler(std::move(h)),
          bindAddr(std::move(addr)),
          port(p) {}

    // Open and bind the socket. Returns false (with a logged reason) instead of
    // throwing, so a bad address or an already-bound port is a clean error
    // rather than an uncaught exception / std::terminate.
    bool open() {
        boost::system::error_code ec;
        const auto address = asio::ip::make_address(bindAddr, ec);
        if (ec) {
            backtest_log::error("UdpReceiver: invalid bind address '" + bindAddr
                                + "': " + ec.message());
            return false;
        }
        const udp::endpoint endpoint(address, port);
        if (const auto openEc = socket.open(endpoint.protocol(), ec); openEc) {
            backtest_log::error("UdpReceiver: socket open failed: " + openEc.message());
            return false;
        }
        if (const auto bindEc = socket.bind(endpoint, ec); bindEc) {
            backtest_log::error("UdpReceiver: cannot bind " + bindAddr + ":"
                                + std::to_string(port) + ": " + bindEc.message());
            return false;
        }
        return true;
    }

    void receive() {
        socket.async_receive_from(
            asio::buffer(buffer), sender,
            [this](const boost::system::error_code& ec, std::size_t n) {
                if (!ec) {
                    handler(std::span<const std::byte>(buffer.data(), n));
                } else if (ec == asio::error::operation_aborted) {
                    return;  // socket closed during shutdown — stop re-arming
                } else {
                    backtest_log::error("UdpReceiver: " + ec.message());
                }
                if (socket.is_open()) {
                    receive();  // re-arm for the next datagram
                }
            });
    }

    void shutdown() {
        boost::system::error_code ec;
        if (const auto closeEc = socket.close(ec); closeEc) {
            backtest_log::error("UdpReceiver: socket close failed: " + closeEc.message());
        }
        ioc.stop();
    }
};

UdpReceiver::UdpReceiver(std::string bindAddr, std::uint16_t port, Handler onDatagram)
    : impl_(std::make_unique<Impl>(std::move(bindAddr), port, std::move(onDatagram))) {}

UdpReceiver::~UdpReceiver() = default;

bool UdpReceiver::run() {
    if (!impl_->open()) {
        return false;  // bind failed; reason already logged
    }
    impl_->signals.async_wait(
        [this](const boost::system::error_code&, int) { impl_->shutdown(); });
    impl_->receive();
    impl_->ioc.run();
    return true;
}

void UdpReceiver::stop() {
    // Hand the teardown to the io_context thread so the socket is only touched
    // from there (Asio objects are not thread-safe for concurrent use).
    asio::post(impl_->ioc, [this] { impl_->shutdown(); });
}

}  // namespace ingest
