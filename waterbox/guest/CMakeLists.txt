# The guest programs, as archives that waterbox/build-guest.sh links into
# .wbx files. Built inside Xenia's build (patch 0001: CHIMERA_DRIVER_DIR) with
# the guest toolchain.

# Upstream's instruction tests, run in one process with the tests named by a
# list (patch 0005), behind one exported call.
add_library(xenia-guest-ppc STATIC
  ${PROJECT_SOURCE_DIR}/src/xenia/cpu/ppc/testing/ppc_testing_main.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/ppc-entry.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/guest-libc.cc
)
target_compile_definitions(xenia-guest-ppc PRIVATE
  XE_SOURCE_ROOT=""
  CHIMERA_INPROCESS_TESTS=1
  XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND)
target_link_libraries(xenia-guest-ppc PUBLIC
  xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu xenia-cpu-backend-x64
  xenia-gpu xenia-gpu-null xenia-hid xenia-hid-nop xenia-kernel xenia-patcher
  xenia-ui xenia-vfs capstone fmt imgui mspack)
xe_target_defaults(xenia-guest-ppc)

# The core: the machine and its exports (waterbox/driver), the same sources the
# native reference (run-frames) is built from.
add_library(xenia-guest-core STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../driver/xenia_driver.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../driver/wbx-entry.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../driver/memfs.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../driver/suggest.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/../driver/title_id.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/guest-libc.cc
)
target_compile_definitions(xenia-guest-core PRIVATE XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND CHIMERA_MEMFS=1)
# vk-bridge.h only: the bridge's own Vulkan headers are not Xenia's
target_include_directories(xenia-guest-core PRIVATE $ENV{MINIBOX_DIR}/source/vk)
target_link_libraries(xenia-guest-core PUBLIC
  xenia-apu xenia-base xenia-core xenia-cpu xenia-cpu-backend-x64
  xenia-gpu xenia-gpu-null xenia-gpu-vulkan
  xenia-hid xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
  aes_128 capstone fmt dxbc imgui libavcodec libavutil mspack snappy xxhash glslang-spirv)
xe_target_defaults(xenia-guest-core)

# The Vulkan bridge's guest half (miniBox, source/vk): the wrappers
# build-guest.sh generated from the registry, and the few commands written by
# hand. Its own archive, with the Vulkan headers the bridge was generated
# from - Xenia's renderer keeps Xenia's. Not named xenia-guest-*: it is one of
# the archives every program links against.
add_library(chimera-vkbridge STATIC
  $ENV{MINIBOX_DIR}/source/vk/vk-guest.c
  ${CHIMERA_VK_GEN}/vk-bridge-guest.c
)
target_include_directories(chimera-vkbridge PRIVATE
  $ENV{MINIBOX_DIR}/source/vk ${CHIMERA_VK_GEN} $ENV{MINIBOX_DIR}/extern/vulkan-headers/include)
