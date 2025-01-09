git submodule update --init --recursive

ls -l ./external/libpqxx

cd ./external/libpqxx

mkdir -p build
cd ./build
cmake .. 
./configure CXXFLAGS="-std=c++20 -O3" --enable-silent-rules
make

ls -l ./