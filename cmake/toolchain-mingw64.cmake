# Cross-compile bsdrX for 64-bit Windows from Linux with MinGW-w64.
#
#   cmake -S . -B build-win \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake \
#     -DOPENSSL_ROOT_DIR=/path/to/openssl-mingw-prefix
#   cmake --build build-win -j
#
# OpenSSL for the mingw target is not packaged by distros; build it once:
#   ./Configure mingw64 no-shared no-tests no-docs no-apps \
#       --cross-compile-prefix=x86_64-w64-mingw32- --prefix=<prefix>
#   make -j build_libs && make install_dev
# then pass that <prefix> as OPENSSL_ROOT_DIR.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# Static libgcc + winpthread so the .exe is self-contained.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc")

set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)

# Link OpenSSL statically (we build it static for mingw).
set(OPENSSL_USE_STATIC_LIBS TRUE)
