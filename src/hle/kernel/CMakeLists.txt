# Xbox kernel HLE (owner: Kernel HLE). Usable from the top-level build
# (add_subdirectory(src/hle/kernel)) or standalone for its unit tests:
#   cmake -S src/hle/kernel -B out/kernel -G Ninja && ninja -C out/kernel -j3
#   out/kernel/rotk_kernel_tests
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(rotk_kernel CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  find_program(CCACHE ccache)
  if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
  endif()
  set(ROTK_STANDALONE ON)
endif()

get_filename_component(ROTK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(X86RECOMP_SRC "${ROTK_ROOT}/third_party/x86recomp/src" CACHE PATH "x86recomp src/ (for runtime/ctx.h)")

add_library(rotk_kernel STATIC
  kernel.cpp memory.cpp time.cpp sync.cpp thread.cpp vfs.cpp file.cpp misc.cpp
  runtime_glue.cpp crash.cpp help.cpp)
target_include_directories(rotk_kernel PUBLIC "${ROTK_ROOT}/src" "${X86RECOMP_SRC}")
target_compile_options(rotk_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-multichar)
# Exports register themselves from static initialisers: keep every object.
if(APPLE)
  target_link_options(rotk_kernel INTERFACE "-Wl,-force_load,$<TARGET_FILE:rotk_kernel>")
endif()

if(ROTK_STANDALONE)
  # Unit tests run on a fake host, without rotk_runtime: no runtime glue.
  target_compile_definitions(rotk_kernel PRIVATE ROTKK_NO_RUNTIME_GLUE=1)
  add_subdirectory(../xinput xinput)
  add_executable(rotk_kernel_tests tests/kernel_tests.cpp)
  target_link_libraries(rotk_kernel_tests PRIVATE rotk_kernel rotk_xinput)
  if(TARGET rotk_xinput_sdl)
    # SDL backend tests on virtual gamepads (no device needed).
    target_link_libraries(rotk_kernel_tests PRIVATE rotk_xinput_sdl)
    target_compile_definitions(rotk_kernel_tests PRIVATE ROTK_TEST_SDL=1)
  endif()
endif()
