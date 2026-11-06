# DirectSound HLE (owner: Audio): the DSOUND section at API level + an SDL3
# mixer. The overrides register from static initialisers: link whole-archive.
add_library(rotk_dsound STATIC
  adpcm.cpp
  dsound.cpp
  engine_fixes.cpp
  mixer.cpp)
target_include_directories(rotk_dsound PRIVATE ${CMAKE_SOURCE_DIR}/src)
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(rotk_dsound PUBLIC rotk_runtime rotk_kernel SDL3::SDL3)
target_compile_options(rotk_dsound PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(APPLE)
  target_link_options(rotk_dsound INTERFACE "-Wl,-force_load,$<TARGET_FILE:rotk_dsound>")
endif()
