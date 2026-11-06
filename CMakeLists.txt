cmake_minimum_required(VERSION 3.25)
project(ROTKPort LANGUAGES C CXX)

# Two layers (CLAUDE.md "Build architecture"):
# - librotk_game.a: lifted game code, built once per lift and published to
#   ROTKPort/.cache/lift/<lift_id>/ (lift/lift.sh); this build only links it.
# - rotk_runtime + HLE libraries + the `rotk` executable: rebuilt in seconds.
#
#   cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   cmake --build out -j3

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Shared ccache (CCACHE_BASEDIR = this worktree, so worktrees share hits).
find_program(CCACHE ccache)
if(CCACHE)
  set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env
      CCACHE_DIR=$ENV{HOME}/Desktop/Claude/ROTKPort/.cache/ccache CCACHE_BASEDIR=${CMAKE_SOURCE_DIR}
      CCACHE_NOHASHDIR=1 ${CCACHE})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_C_COMPILER_LAUNCHER})
endif()

# No build paths in the binaries (the DMG kit ships our libraries; infra publish audit 2026-11-06):
# __FILE__ and debug info name files relative to the repo.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=)
include(cmake/x86recomp.cmake)

# The published lift this worktree is pinned to (lift/LIFT_ID).
file(STRINGS "${CMAKE_SOURCE_DIR}/lift/LIFT_ID" _lift_line REGEX "^LIFT_ID=")
string(REGEX REPLACE "^LIFT_ID=" "" ROTK_LIFT_ID "${_lift_line}")
set(ROTK_LIFT_ROOT "$ENV{HOME}/Desktop/Claude/ROTKPort/.cache/lift" CACHE PATH "")
# ROTK_LIFT_DIR overrides the pin (e.g. -DROTK_LIFT_DIR=/path/to/a/lift); not
# cached, so a new lift/LIFT_ID takes effect on the next configure.
if(NOT ROTK_LIFT_DIR)
  set(ROTK_LIFT_DIR "${ROTK_LIFT_ROOT}/${ROTK_LIFT_ID}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/lift/LIFT_ID")

add_subdirectory(src/runtime)
add_subdirectory(src/loader)
