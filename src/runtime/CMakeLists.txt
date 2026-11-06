# rotk_runtime: loader core, dispatch (overrides, kernel/host thunks), guest
# threads, logging. Public API for HLE roles: rotk_runtime.h.
add_library(rotk_runtime STATIC
  rotk_runtime.cpp
  rotk_dispatch.cpp
  rotk_thread.cpp
  rotk_boot.cpp
  rotk_game_fixes.cpp
  # x86recomp's platform layer: fault context decoding and the signal-frame
  # redirect used to leave a fault with a guest exception.
  ${X86RECOMP_DIR}/src/runtime/platform.cpp)
target_include_directories(rotk_runtime PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(rotk_runtime PUBLIC recomp_x87 recomp_image)
target_compile_options(rotk_runtime PRIVATE -Wall -Wno-unused-function)

# MMX lane helpers (x86recomp patch) against hand-checked values.
add_executable(rotk_mmx_test tests/mmx_test.cpp)
target_link_libraries(rotk_mmx_test PRIVATE recomp_x87)

# SSE host fast path (patches/x86recomp-sse-fastpath.patch) against the exact
# SoftFloat path: same result bits. out/src/runtime/rotk_sse_fast_test [n]
add_executable(rotk_sse_fast_test ${X86RECOMP_DIR}/tests/sse_fast_test.cpp)
target_link_libraries(rotk_sse_fast_test PRIVATE recomp_x87)
target_compile_options(rotk_sse_fast_test PRIVATE -O2 ${RECOMP_X87_FP_FLAGS})

# XboxPortKit donor gate: title/overrides.allow
# The list is built into the binary (a player's Mac has no repo; no build path in the binary):
# overrides_allow.inc = the file as a raw string literal, regenerated when the file changes.
file(READ "${CMAKE_SOURCE_DIR}/title/overrides.allow" _rotk_allow)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/overrides_allow.inc"
     CONTENT "R\"ALLOW(${_rotk_allow})ALLOW\"\n" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/title/overrides.allow")
target_include_directories(rotk_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
