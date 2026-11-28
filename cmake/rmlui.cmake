# RmlUi (third_party/rmlui, MIT) with its FreeType font engine (third_party/freetype, FTL): the interface
# (libverdite's verdite_ui, port/ui/). Both copied into the repository, as libchdr is, so a release builds without
# the network. Third-party code: no warnings, optimised in every build type, SYSTEM includes so lint skips them.
set(VERDITE_FREETYPE ${CMAKE_CURRENT_LIST_DIR}/../third_party/freetype)
add_library(verdite_freetype STATIC
  ${VERDITE_FREETYPE}/src/base/ftbase.c
  ${VERDITE_FREETYPE}/src/base/ftinit.c
  ${VERDITE_FREETYPE}/src/base/ftsystem.c
  ${VERDITE_FREETYPE}/src/base/ftdebug.c
  ${VERDITE_FREETYPE}/src/base/ftbitmap.c
  ${VERDITE_FREETYPE}/src/base/ftglyph.c
  ${VERDITE_FREETYPE}/src/base/ftmm.c
  ${VERDITE_FREETYPE}/src/base/ftstroke.c
  ${VERDITE_FREETYPE}/src/base/ftsynth.c
  ${VERDITE_FREETYPE}/src/truetype/truetype.c
  ${VERDITE_FREETYPE}/src/sfnt/sfnt.c
  ${VERDITE_FREETYPE}/src/smooth/smooth.c
  ${VERDITE_FREETYPE}/src/autofit/autofit.c
  ${VERDITE_FREETYPE}/src/psnames/psnames.c
  ${VERDITE_FREETYPE}/src/cff/cff.c
  ${VERDITE_FREETYPE}/src/psaux/psaux.c
  ${VERDITE_FREETYPE}/src/pshinter/pshinter.c)
target_include_directories(verdite_freetype SYSTEM PUBLIC ${VERDITE_FREETYPE}/include PRIVATE ${VERDITE_FREETYPE})
target_compile_definitions(verdite_freetype PRIVATE FT2_BUILD_LIBRARY
  "FT_CONFIG_MODULES_H=<config/ftmodule.h>" "FT_CONFIG_OPTIONS_H=<config/ftoption.h>")
target_compile_options(verdite_freetype PRIVATE -w -O2)
set_target_properties(verdite_freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(VERDITE_RMLUI ${CMAKE_CURRENT_LIST_DIR}/../third_party/rmlui)
file(GLOB_RECURSE VERDITE_RMLUI_SOURCES CONFIGURE_DEPENDS ${VERDITE_RMLUI}/Source/Core/*.cpp)
add_library(verdite_rmlui STATIC ${VERDITE_RMLUI_SOURCES})
target_include_directories(verdite_rmlui SYSTEM PUBLIC ${VERDITE_RMLUI}/Include)
target_compile_definitions(verdite_rmlui PUBLIC RMLUI_STATIC_LIB
  PRIVATE RMLUI_FONT_ENGINE_FREETYPE "RMLUI_VERSION=\"6.3\"")
target_link_libraries(verdite_rmlui PRIVATE verdite_freetype)
target_compile_options(verdite_rmlui PRIVATE -w -O2)
set_target_properties(verdite_rmlui PROPERTIES CXX_STANDARD 17 POSITION_INDEPENDENT_CODE ON)
