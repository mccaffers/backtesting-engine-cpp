git submodule update --init --recursive

cd ./external/libpqxx

mkdir -p build
cd ./build

# Expose paths so CMake finds libpq
if command -v brew &>/dev/null; then
    export PATH="$(brew --prefix libpq)/bin:$PATH"
    export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:$PKG_CONFIG_PATH"
    export PostgreSQL_ROOT="$(brew --prefix libpq)"
fi

# 1. Generate build files (Passing your CXX flags directly to CMake instead of configure)
cmake .. \
  -DCMAKE_CXX_STANDARD=20 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSKIP_BUILD_TEST=ON

JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$JOBS"