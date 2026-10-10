# The miniBox guest as a CMake toolchain: musl and a static libstdc++, the
# large code model, no position-independent code, nothing of the host's.
# MINIBOX_DIR names a miniBox checkout built with the C++ guest toolchain
# (meson setup <miniBox>/build/meson-cpp <miniBox> -Dguest_cpp=true).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(DEFINED ENV{MINIBOX_SYSROOT})
  set(SR "$ENV{MINIBOX_SYSROOT}")
elseif(DEFINED ENV{MINIBOX_DIR})
  set(SR "$ENV{MINIBOX_DIR}/build/meson-cpp/guest-sysroot")
else()
  message(FATAL_ERROR "set MINIBOX_DIR to a miniBox checkout (or MINIBOX_SYSROOT to its guest sysroot)")
endif()
if(NOT EXISTS "${SR}/lib/musl-gcc.specs" OR NOT EXISTS "${SR}/lib/libstdc++.a")
  message(FATAL_ERROR "no C++ guest toolchain at ${SR}: meson setup <miniBox>/build/meson-cpp <miniBox> -Dguest_cpp=true && ninja -C <miniBox>/build/meson-cpp")
endif()

set(CMAKE_C_COMPILER gcc)
set(CMAKE_CXX_COMPILER g++)
execute_process(COMMAND gcc -dumpfullversion OUTPUT_VARIABLE GCCVER OUTPUT_STRIP_TRAILING_WHITESPACE)

set(WB "-specs=${SR}/lib/musl-gcc.specs -fvisibility=hidden -mcmodel=large -mstack-protector-guard=global -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -DCHIMERA_GUEST")
set(CMAKE_C_FLAGS_INIT "${WB}")
set(CMAKE_CXX_FLAGS_INIT "${WB} -nostdinc++ -I${SR}/include/c++/${GCCVER} -I${SR}/include/c++/${GCCVER}/x86_64-linux-musl")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -nostdlib++")

# the host's pkg-config would hand the guest the host's libraries
set(PKG_CONFIG_EXECUTABLE "/bin/false" CACHE FILEPATH "no host pkg-config for the guest" FORCE)
# feature probes must not try to run guest binaries
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${SR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
