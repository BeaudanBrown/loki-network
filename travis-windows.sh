pacman -Sy --noconfirm base-devel mingw-w64-x86_64-toolchain git libtool autoconf mingw-w64-x86_64-cmake curl mingw-w64-x86_64-ninja
cmake .. -DCMAKE_BUILD_TYPE=Debug -DSTATIC_LINK=ON -G Ninja
ninja
./testAll.exe
