# XInput HLE (owner: Kernel HLE): rotk_xinput (game-facing, no SDL) and
# rotk_xinput_sdl (SDL3 gamepad/keyboard backend).
add_library(rotk_xinput STATIC xinput.cpp pad_script.cpp input_filter.cpp)
target_link_libraries(rotk_xinput PUBLIC rotk_kernel)
target_compile_options(rotk_xinput PRIVATE -Wall -Wextra -Wno-unused-parameter)

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(rotk_xinput_sdl STATIC xinput_sdl.cpp)
  target_link_libraries(rotk_xinput_sdl PUBLIC rotk_xinput SDL3::SDL3)
endif()
