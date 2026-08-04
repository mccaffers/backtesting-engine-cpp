// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

// A tiny shared-memory control channel between the C++ engine and a local Python
// monitor (source/backtesting-controller). The engine broadcasts how many
// backtests are in flight; the monitor can flip a stop flag to ask for a graceful
// drain-and-pause. Zero network, zero serialisation — both sides map the same
// 12-byte block (see EngineState below: magic + active_jobs + stop_signal; the
// Python side must map all 12 bytes and read the fields at offsets 4 and 8).
//
// The block is a memory-mapped FILE at a fixed path (DEFAULT_PATH below), mapped
// via Boost.Interprocess `file_mapping`. We use a file rather than
// `shared_memory_object` because the latter is not portable to a `mmap`-based
// Python reader on macOS: there, Boost falls back to a file under its own private
// directory while Python's `multiprocessing.shared_memory` only speaks `shm_open`,
// so the two never meet. A shared file path is deterministic and identical on
// macOS and Linux; the Python side just `mmap`s the same path.
//
// Boost.Interprocess is confined to engineControl.cpp (see the boostRedisImpl.cpp
// pattern) so includers of this header stay free of Boost headers and macros.
namespace ipc {

// A sentinel written at offset 0 so a reader can prove it mapped *this* engine's
// control block before trusting the rest of the bytes — not a stale file from an
// older struct layout, a half-initialised file, or some unrelated file that
// happens to sit at the same path.
inline constexpr std::uint32_t MAGIC = 0xDEADBEEFu;

// The exact bytes mapped into both processes: a magic sentinel followed by two
// naturally-aligned, always lock-free 32-bit atomics, 12 bytes total with no
// padding:
//   [0,  4)  magic        — fixed 0xDEADBEEF identity/layout marker
//   [4,  8)  active_jobs  — C++ writes, Python reads
//   [8, 12)  stop_signal  — Python writes (0 = run, 1 = stop), C++ reads
// A single aligned read/write of each field is atomic on x86-64 and arm64, so the
// Python side can use raw `struct` access on the corresponding byte ranges.
struct EngineState {
    const std::uint32_t magic = MAGIC;
    std::atomic<std::int32_t> active_jobs;
    std::atomic<std::int32_t> stop_signal;
};

static_assert(std::atomic<std::int32_t>::is_always_lock_free,
              "EngineState relies on lock-free atomics for cross-process access");
static_assert(sizeof(EngineState) == 12, "EngineState must be a packed 12-byte block");
static_assert(alignof(EngineState) == 4, "EngineState fields must be 4-byte aligned");

// The default backing file. The Python controller defaults to the same path.
// Safe to use /tmp: Environment is a private, single-use machine
inline constexpr auto DEFAULT_PATH = "/tmp/EngineControlShm"; // NOSONAR

// RAII owner of the mapped file. Construction creates and zeroes the file
// (replacing any stale one left by a crashed run); destruction unmaps and removes
// it. Throws on failure — callers that treat the channel as best-effort should
// construct it inside a try/catch.
class EngineControlChannel {
public:
    explicit EngineControlChannel(std::string path = DEFAULT_PATH);
    ~EngineControlChannel();

    EngineControlChannel(const EngineControlChannel&) = delete;
    EngineControlChannel& operator=(const EngineControlChannel&) = delete;

    // The live, mapped state. Valid for the lifetime of this object.
    EngineState& state() noexcept { return *state_; }

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    struct Impl;  // hides the Boost.Interprocess members
    std::unique_ptr<Impl> impl_;
    EngineState* state_ = nullptr;
};

}  // namespace ipc
