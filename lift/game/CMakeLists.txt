# librotk_game.a: the lifted game code (lift/lift.sh builds it once per lift,
# under the `heavy` lock, and publishes it to ROTKPort/.cache/lift/<lift_id>/).
#
#   cmake -S lift/game -B <dir> -G Ninja -DROTK_GEN_DIR=<generated> -DX86RECOMP_DIR=<x86recomp>
cmake_minimum_required(VERSION 3.25)
project(rotk_game LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(CCACHE ccache)
if(CCACHE)
  # Keyed by content: the same generated TU in another lift hits.
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env
      CCACHE_DIR=$ENV{HOME}/Desktop/Claude/ROTKPort/.cache/ccache CCACHE_BASEDIR=${ROTK_GEN_DIR}
      CCACHE_NOHASHDIR=1 ${CCACHE})
endif()

file(GLOB _gen "${ROTK_GEN_DIR}/*.cpp")
if(NOT _gen)
  message(FATAL_ERROR "no generated code in ${ROTK_GEN_DIR}")
endif()
add_library(rotk_game STATIC ${_gen})
target_include_directories(rotk_game PRIVATE "${X86RECOMP_DIR}/src")
# -O3: in-level frames 6-8% faster than -O2 (3.59 -> 3.35 ms unpaced, M1 Pro,
# 2026-10-31), phase-3 goldens bit-identical. Otherwise as x86recomp's
# recomp_add_lifted: always optimised; guest exceptions
# (GuestResume, the kernel's thread exit) unwind through lifted frames;
# exact x87 doubles; frame records for fault reports on Apple hosts.
target_compile_options(rotk_game PRIVATE -O3 -g0 -fno-strict-aliasing -fnon-call-exceptions
                       -ffp-contract=off -fno-fast-math
                       $<$<PLATFORM_ID:Darwin>:-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer>)
