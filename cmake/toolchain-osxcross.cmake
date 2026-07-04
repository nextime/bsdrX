# Cross-compile bsdrX for macOS from Linux with osxcross.
#
# macOS cross-compilation needs Apple's SDK (extracted from Xcode on a Mac — a
# licensing requirement, so it can't be bundled here). One-time setup:
#   1. git clone https://github.com/tpoechtrager/osxcross
#   2. Provide a MacOSXSDK tarball (packaged from Xcode) under osxcross/tarballs/
#   3. cd osxcross && ./build.sh        # builds the toolchain under target/
#   4. export OSXCROSS_ROOT=/path/to/osxcross/target
#      export PATH=$OSXCROSS_ROOT/bin:$PATH
#
# Then:
#   cmake -S . -B build-mac \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-osxcross.cmake \
#     -DOPENSSL_ROOT_DIR=<openssl-built-for-darwin>
#   cmake --build build-mac -j
#
# The CoreGraphics/CoreFoundation frameworks the injector uses come from the SDK
# inside osxcross, so no extra setup beyond the SDK is required.

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)        # set to arm64 for Apple Silicon targets

if(NOT DEFINED ENV{OSXCROSS_ROOT})
  message(FATAL_ERROR "Set OSXCROSS_ROOT to your osxcross target/ directory")
endif()
set(OSXCROSS_ROOT $ENV{OSXCROSS_ROOT})

# osxcross names tools like x86_64-apple-darwinXX-clang; the wrappers o64-clang
# / oa64-clang also work. Adjust the darwin version to your SDK if needed.
set(CMAKE_C_COMPILER   ${OSXCROSS_ROOT}/bin/o64-clang)
set(CMAKE_CXX_COMPILER ${OSXCROSS_ROOT}/bin/o64-clang++)

set(CMAKE_FIND_ROOT_PATH ${OSXCROSS_ROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
