# Polish: optional enhancements over the lifted game (widescreen, the TV 2002 look, ...).
# Linked whole-archive: ROTK_OVERRIDEs register from static initialisers.
enable_language(OBJCXX)
add_library(rotk_polish STATIC widescreen.cpp skip.cpp subs.cpp fps.cpp postfx/postfx.mm texpack/texpack.mm texpack/prompts.mm)
target_include_directories(rotk_polish PRIVATE "${CMAKE_SOURCE_DIR}/src" "${X86RECOMP_DIR}/src")
find_package(SDL3 CONFIG REQUIRED)
target_link_libraries(rotk_polish PRIVATE rotk_runtime SDL3::SDL3 "-framework Metal" "-framework Foundation"
  "-framework CoreGraphics" "-framework CoreText" "-framework ImageIO")
target_compile_options(rotk_polish PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc -Wall>)
# rotk_d3d8_set_widescreen / _postfx / _texture_hook (C++, mangled) are weak: resolved when
# the D3D8 HLE provides them.
target_link_options(rotk_polish INTERFACE "-Wl,-U,_rotk_d3d8_set_widescreen" "-Wl,-U,_rotk_d3d8_set_postfx"
  "-Wl,-U,__Z25rotk_d3d8_set_texture_hookPFbRK14RotkTextureInfoPjS2_PPKhE")
