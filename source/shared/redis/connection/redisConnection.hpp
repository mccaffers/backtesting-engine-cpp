// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/redis/connection.hpp>

namespace redis_util {

std::shared_ptr<boost::redis::connection> makeRedisConnection(
    boost::asio::io_context& ioc,
    const std::string& host,
    int port);

}  // namespace redis_util
