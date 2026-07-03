// Backtesting Engine in C++
//
// (c) 2026 Ryan McCaffery | https://mccaffers.com
// This code is licensed under MIT license (see LICENSE.txt for details)
// ---------------------------------------

#include "shared/ipc/engineControl.hpp"

#include <filesystem>
#include <fstream>
#include <new>
#include <utility>

#include <boost/interprocess/file_mapping.hpp>
#include <boost/interprocess/mapped_region.hpp>

// This is the one TU that names Boost.Interprocess. It mirrors the isolation
// pattern of boostRedisImpl.cpp: the heavy, macro-laden header is included here
// and nowhere else, behind the pimpl declared in engineControl.hpp.
namespace bip = boost::interprocess;
namespace fs = std::filesystem;

namespace ipc {

struct EngineControlChannel::Impl {
    bip::file_mapping file;
    bip::mapped_region region;
};

EngineControlChannel::EngineControlChannel(std::string path)
    : path_(std::move(path)), impl_(std::make_unique<Impl>()) {
    // Recreate the backing file from scratch so a segment left behind by a
    // previous (crashed) run can't carry stale state forward. (Same intent as
    // the struct_shm_remove guard in the reference snippet.)
    const fs::path file_path(path_);
    if (file_path.has_parent_path()) {
        fs::create_directories(file_path.parent_path());
    }
    { std::ofstream create(path_, std::ios::binary | std::ios::trunc); }
    fs::resize_file(file_path, sizeof(EngineState));

    impl_->file = bip::file_mapping(path_.c_str(), bip::read_write);
    impl_->region = bip::mapped_region(impl_->file, bip::read_write);

    // Placement-new the atomics into the mapped bytes, then publish a known-good
    // initial state (idle, running).
    state_ = ::new (impl_->region.get_address()) EngineState();
    state_->active_jobs.store(0, std::memory_order_relaxed);
    state_->stop_signal.store(0, std::memory_order_relaxed);
}

EngineControlChannel::~EngineControlChannel() {
    state_ = nullptr;
    impl_.reset();  // unmap + close before removing the file
    std::error_code ec;
    fs::remove(fs::path(path_), ec);  // best-effort; ignore if already gone
}

}  // namespace ipc
