# rotk: the executable. Links the published lift this worktree pins
# (ROTK_LIFT_DIR/lib/librotk_game.a) and, when present, the HLE libraries
# (whole-archive: their exports and ROTK_OVERRIDEs register from static
# initialisers and override the runtime's weak fallbacks).
set(ROTK_KERNEL_DIR "${CMAKE_SOURCE_DIR}/src/hle/kernel" CACHE PATH
    "Kernel HLE sources (src/hle/kernel, or a checkout of its branch)")
set(_game_lib "${ROTK_LIFT_DIR}/lib/librotk_game.a")
if(NOT EXISTS "${_game_lib}")
  message(WARNING "no published lift at ${ROTK_LIFT_DIR} (lift/LIFT_ID=${ROTK_LIFT_ID}); "
                  "run lift/lift.sh --pin. The rotk executable is not configured.")
  return()
endif()
add_executable(rotk main.cpp)
target_link_libraries(rotk PRIVATE rotk_runtime "${_game_lib}")
# Kernel HLE hooks the runtime/loader use when present (weak: crash reports,
# --help text); resolved by rotk_kernel when it is linked.
target_link_options(rotk PRIVATE "-Wl,-U,__Z14rotk_crash_notePKc" "-Wl,-U,__Z16rotk_crash_reportPKc"
                    "-Wl,-U,__Z13rotk_help_textv")
if(EXISTS "${ROTK_KERNEL_DIR}/CMakeLists.txt")
  set(X86RECOMP_SRC "${X86RECOMP_DIR}/src" CACHE PATH "" FORCE)
  add_subdirectory("${ROTK_KERNEL_DIR}" "${CMAKE_BINARY_DIR}/hle_kernel")
  # This tree's runtime header, also when the kernel comes from elsewhere.
  target_include_directories(rotk_kernel PRIVATE "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(rotk PRIVATE rotk_kernel)
  message(STATUS "rotk: kernel HLE from ${ROTK_KERNEL_DIR}")
  # XInput HLE (game-facing part; the SDL backend joins with the frame loop).
  if(EXISTS "${ROTK_KERNEL_DIR}/../xinput/CMakeLists.txt")
    add_subdirectory("${ROTK_KERNEL_DIR}/../xinput" "${CMAKE_BINARY_DIR}/hle_xinput")
    target_link_libraries(rotk PRIVATE rotk_xinput)
    target_compile_definitions(rotk PRIVATE ROTK_HAVE_XINPUT=1)
  endif()
  # D3D8 HLE (Render): needs the kernel's vblank pacer.
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/hle/d3d8/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/hle/d3d8" "${CMAKE_BINARY_DIR}/hle_d3d8")
    target_link_libraries(rotk PRIVATE rotk_d3d8)
  endif()
  # DirectSound HLE (Audio): needs the kernel (NtSetEvent for stream packets).
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/hle/dsound/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/hle/dsound" "${CMAKE_BINARY_DIR}/hle_dsound")
    target_link_libraries(rotk PRIVATE rotk_dsound)
  endif()
  # Polish (opt-in enhancements: ROTK_WIDESCREEN, ...).
  if(EXISTS "${CMAKE_SOURCE_DIR}/src/polish/CMakeLists.txt")
    add_subdirectory("${CMAKE_SOURCE_DIR}/src/polish" "${CMAKE_BINARY_DIR}/polish")
    target_link_libraries(rotk PRIVATE "-Wl,-force_load,$<TARGET_FILE:rotk_polish>" rotk_polish)
  endif()
else()
  message(STATUS "rotk: no kernel HLE (runtime stops at the first kernel call)")
endif()
