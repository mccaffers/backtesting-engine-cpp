cd ./external/libpqxx

mkdir -p build
cd ./build

export PATH="$(brew --prefix libpq)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix libpq)/lib/pkgconfig:$PKG_CONFIG_PATH"
export PostgreSQL_ROOT="$(brew --prefix libpq)"

cmake .. -DCMAKE_CXX_STANDARD=20 -DCMAKE_BUILD_TYPE=Release 
make