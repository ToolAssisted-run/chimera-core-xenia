# The core's own programs, built inside Xenia's build (patch 0001 adds this
# directory when CHIMERA_DRIVER_DIR names it) so that they see its targets
# and its flags.
add_executable(run-native
  ${CMAKE_CURRENT_SOURCE_DIR}/run-native.cc
  ${PROJECT_SOURCE_DIR}/src/xenia/base/console_app_main_posix.cc
)
target_compile_definitions(run-native PRIVATE XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND)
target_link_libraries(run-native PRIVATE
  xenia-apu xenia-apu-nop xenia-base xenia-core xenia-cpu
  xenia-gpu xenia-gpu-null xenia-gpu-vulkan
  xenia-hid xenia-hid-nop xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
  aes_128 capstone fmt dxbc imgui libavcodec libavutil mspack snappy xxhash glslang-spirv
)
if(XE_TARGET_X86_64)
  target_link_libraries(run-native PRIVATE xenia-cpu-backend-x64)
endif()
xe_target_defaults(run-native)

# The core's exports as a native program, a frame at a time: the reference
# the sandboxed core is compared with. Only where a frame is a stretch of the
# machine's own clock.
if(CHIMERA_VSCHED)
  add_executable(run-frames
    ${CMAKE_CURRENT_SOURCE_DIR}/run-frames.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/xenia_driver.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/wbx-entry.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../driver/memfs.cc
  )
  target_compile_definitions(run-frames PRIVATE XBYAK_NO_OP_NAMES XBYAK_ENABLE_OMITTED_OPERAND)
  target_link_libraries(run-frames PRIVATE
    xenia-apu xenia-base xenia-core xenia-cpu
    xenia-gpu xenia-gpu-null xenia-gpu-vulkan
    xenia-hid xenia-kernel xenia-patcher xenia-ui xenia-ui-vulkan xenia-vfs
    aes_128 capstone fmt dxbc imgui libavcodec libavutil mspack snappy xxhash glslang-spirv
  )
  if(XE_TARGET_X86_64)
    target_link_libraries(run-frames PRIVATE xenia-cpu-backend-x64)
  endif()
  xe_target_defaults(run-frames)
endif()
