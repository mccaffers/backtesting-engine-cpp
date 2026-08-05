// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cstdint>

// The C# streamer (vortex/shared/UDPPorts.cs) fans its serialized feeds out to
// several UDP sinks. This engine consumes three of them: the persistence
// stream (ingest -> QuestDB) and the live stream (live -> strategy execution),
// both carrying 40-byte tick packets, plus the deal stream (tracking ->
// position updates) carrying 256-byte deal packets. Keep these in lockstep
// with UDPPorts.cs. Each bind port is also overridable at runtime (CLI arg /
// $INGEST_UDP_PORT / $LIVE_UDP_PORT / $TRACKING_UDP_PORT).
namespace udp_ports {

inline constexpr std::uint16_t kSave  = 11111;  // == UDPPorts.PortSave
inline constexpr std::uint16_t kLive  = 11110;  // == UDPPorts.PortLive
inline constexpr std::uint16_t kTrade = 11112;  // == UDPPorts.PortTrade

}  // namespace udp_ports
