// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <cstdint>

// The C# streamer (vortex/shared/UDPPorts.cs) fans each serialized tick out to
// several UDP sinks. This engine only consumes the persistence stream, so the
// ingest cares about exactly one port: PortSave. Keep kSave in lockstep with
// UDPPorts.PortSave. The bind port is also overridable at runtime
// (CLI arg / $INGEST_UDP_PORT).
namespace udp_ports {

inline constexpr std::uint16_t kSave = 11111;  // == UDPPorts.PortSave

}  // namespace udp_ports
