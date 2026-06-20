#!/usr/bin/env bash
# Install the Ubuntu CI toolchain: Clang + libc++ from apt.llvm.org (the Linux
# equivalent of the macOS Homebrew-LLVM setup), Ninja, the engine's system
# dependencies, and a Boost new enough for Boost.Redis. Mirrors the toolchain
# scripts/build.sh expects so `import std` works the same on both platforms.
set -euo pipefail

LLVM_VERSION="${LLVM_VERSION:-20}"
BOOST_VERSION="${BOOST_VERSION:-1.90.0}"

echo "== Installing Clang ${LLVM_VERSION} + libc++ from apt.llvm.org =="
wget -q https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh "${LLVM_VERSION}"

sudo apt-get update
sudo apt-get install -y \
    "libc++-${LLVM_VERSION}-dev" "libc++abi-${LLVM_VERSION}-dev" \
    "libomp-${LLVM_VERSION}-dev" \
    ninja-build \
    libssl-dev libpq-dev libcurl4-openssl-dev

# Make the pinned Clang the default `clang`/`clang++` so scripts/build.sh and the
# build-wrapper pick it up via the unversioned names.
sudo update-alternatives --install /usr/bin/clang   clang   "/usr/bin/clang-${LLVM_VERSION}"   100
sudo update-alternatives --install /usr/bin/clang++ clang++ "/usr/bin/clang++-${LLVM_VERSION}" 100

# update-alternatives doesn't reliably repoint /usr/bin/clang on the runner (the
# image ships its own system Clang 18 at that path), so scripts/build.sh fell
# back to it — and libomp-18-dev is never installed, breaking
# find_package(OpenMP REQUIRED). Pin $CC/$CXX to the versioned binaries for every
# later workflow step; build.sh honours them over the unversioned fallback.
if [ -n "${GITHUB_ENV:-}" ]; then
    echo "CC=clang-${LLVM_VERSION}"   >> "$GITHUB_ENV"
    echo "CXX=clang++-${LLVM_VERSION}" >> "$GITHUB_ENV"
fi

# Ubuntu's apt Boost predates Boost.Redis (added in 1.84). Boost.Redis/Asio are
# header-only, so only the headers + CMake config are needed — building just
# Boost.System generates both quickly. (-d0 silences per-action output.)
echo "== Installing Boost ${BOOST_VERSION} (for Boost.Redis) =="
BOOST_DIR="boost_${BOOST_VERSION//./_}"
wget -q "https://archives.boost.io/release/${BOOST_VERSION}/source/${BOOST_DIR}.tar.gz"
tar xzf "${BOOST_DIR}.tar.gz"
cd "${BOOST_DIR}"
./bootstrap.sh > /dev/null
sudo ./b2 install -d0 --prefix=/usr/local --with-system -j"$(nproc)"
