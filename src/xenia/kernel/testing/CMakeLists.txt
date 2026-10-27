xe_test_suite(xenia-kernel-tests ${CMAKE_CURRENT_SOURCE_DIR}
  LINKS
    capstone fmt imgui mbedtls
    xenia-apu xenia-base xenia-core xenia-cpu xenia-gpu
    xenia-hid-portal xenia-kernel xenia-ui xenia-patcher
)
if(XE_TARGET_X86_64 AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-cpu-backend-x64)
elseif(XE_TARGET_AARCH64 AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-cpu-backend-a64)
endif()
# xenia-apu pulls in xenia-apu-sdl on Linux (see src/xenia/apu/CMakeLists.txt).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET xenia-kernel-tests)
  target_link_libraries(xenia-kernel-tests PRIVATE xenia-apu-sdl)
endif()
