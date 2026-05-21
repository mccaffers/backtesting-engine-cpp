// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "redisConnection.hpp"

#include <memory>
#include <string>

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/redis/config.hpp>

namespace redis_util {

std::shared_ptr<boost::redis::connection> makeRedisConnection(
    boost::asio::io_context& ioc,
    const std::string& host,
    int port) {
    auto conn = std::make_shared<boost::redis::connection>(ioc);

    boost::redis::config cfg;
    cfg.addr.host = host;
    cfg.addr.port = std::to_string(port);

    conn->async_run(cfg, {},
                    boost::asio::consign(boost::asio::detached, conn));

    return conn;
}

}  // namespace redis_util
