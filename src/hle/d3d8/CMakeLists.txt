# D3D8 HLE (owner: Render): the D3D section functions on Metal.
# The overrides register from static initialisers: link whole-archive.
enable_language(OBJCXX)
add_library(rotk_d3d8 STATIC
  d3d8.cpp
  vsh.cpp
  psh.cpp
  textures.cpp
  movies.cpp
  window.mm
  video_settings.cpp
  gpu_metal.mm)
target_include_directories(rotk_d3d8 PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(rotk_d3d8 PUBLIC rotk_runtime rotk_kernel rotk_xinput_sdl SDL3::SDL3
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework CoreGraphics" "-framework AppKit"
  "-framework ImageIO")
target_compile_options(rotk_d3d8 PRIVATE -Wall -Wno-unused-function
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
if(APPLE)
  target_link_options(rotk_d3d8 INTERFACE "-Wl,-force_load,$<TARGET_FILE:rotk_d3d8>")
endif()

add_executable(rotk_d3d8_video_settings_test tests/video_settings_test.cpp video_settings.cpp)
target_include_directories(rotk_d3d8_video_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(COMMAND add_test)
  add_test(NAME d3d8_video_settings COMMAND rotk_d3d8_video_settings_test)
endif()
