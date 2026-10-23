cmake_minimum_required(VERSION 3.25)
project(kings_field VERSION 0.1.0 LANGUAGES C CXX)
# Verdite1 on the decomp. The layout follows holyland:
#   decomp/       sushi-shi's source port (our fork), a submodule pinned on `port`; never edited
#   patches/      Verdite's edits to its game code, one diff per copied directory (tools/patches.py)
#   port/         the platform layer, renderer and PC extensions: Verdite's own, edited in place
#   libverdite/   the game-agnostic C++ library the Verdite ports share, to become a subtree; knows no game
#   third_party/  imgui, lua, cjson, the GLES headers and loader
# At configure time the decomp's game code (KF_COPIED) is copied, the patches are applied to the
# copy, and the copy (build/<preset>/decomp_src/) is compiled with port/.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
option(KF_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
if(KF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- The decomp's game code, patched ------------------------------------------------------------
# From holyland: each configure builds the patched decomp in a staging directory, then copies into
# decomp_src/ only the files whose content changed, so a patch edit recompiles only what it touches.
# KF_COPIED must match COPIED in tools/patches.py.
set(KF_COPIED include/kf/audio include/kf/cutscene include/kf/game include/kf/lib
    src/audio src/cutscene src/game src/lib)
set(KF_DECOMP ${CMAKE_SOURCE_DIR}/decomp)
set(KF_STAGE ${CMAKE_BINARY_DIR}/decomp_stage)
set(KF_SRC ${CMAKE_BINARY_DIR}/decomp_src)
if(NOT EXISTS ${KF_DECOMP}/src/game)
  message(FATAL_ERROR "decomp/src/game is missing: run git submodule update --init")
endif()
find_package(Git REQUIRED)

file(GLOB_RECURSE KF_PATCHES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/patches/*.patch)
list(SORT KF_PATCHES)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KF_PATCHES})
# moving the decomp pin re-runs configure: git's HEAD file of the submodule changes with it
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${KF_DECOMP} rev-parse --git-path HEAD
  OUTPUT_VARIABLE KF_DECOMP_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(rc EQUAL 0)
  get_filename_component(KF_DECOMP_HEAD ${KF_DECOMP_HEAD} ABSOLUTE BASE_DIR ${KF_DECOMP})
  if(EXISTS ${KF_DECOMP_HEAD})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${KF_DECOMP_HEAD})
  endif()
endif()

# decomp/ must be at its pin, unedited: an edit there would be compiled without being in a patch
execute_process(COMMAND ${GIT_EXECUTABLE} -C ${KF_DECOMP} status --porcelain
  OUTPUT_VARIABLE KF_DECOMP_EDITS OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "git status failed in ${KF_DECOMP}:\n${out}")
endif()
if(NOT "${KF_DECOMP_EDITS}" STREQUAL "")
  message(FATAL_ERROR "decomp/ has local changes:\n${KF_DECOMP_EDITS}\n"
    "decomp/ must stay at its pin, unedited. Game-code edits go in patches/ (tools/patches.py).")
endif()

# the staging copy: the pinned decomp with the patches applied, rebuilt from scratch every configure
file(REMOVE_RECURSE ${KF_STAGE})
foreach(dir ${KF_COPIED})
  get_filename_component(parent ${KF_STAGE}/${dir} DIRECTORY)
  file(MAKE_DIRECTORY ${parent})
  file(COPY ${KF_DECOMP}/${dir} DESTINATION ${parent})
endforeach()
# its own repo, so that git apply works on paths relative to the copy's root (not this repo's)
execute_process(COMMAND ${GIT_EXECUTABLE} init -q WORKING_DIRECTORY ${KF_STAGE} RESULT_VARIABLE rc
  OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "git init failed in ${KF_STAGE}:\n${out}")
endif()
foreach(patch ${KF_PATCHES})
  execute_process(COMMAND ${GIT_EXECUTABLE} apply --whitespace=nowarn ${patch}
    WORKING_DIRECTORY ${KF_STAGE} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "patch ${patch} does not apply to decomp/ at its pin:\n${out}")
  endif()
endforeach()
file(REMOVE_RECURSE ${KF_STAGE}/.git)

# sync the staging copy into decomp_src/. COPY_FILE leaves a file alone when it is already the
# same, so its timestamp, and the object built from it, survive. decomp_src/ is kept read-only
# between configures (see below), so make it writable for the sync.
if(EXISTS ${KF_SRC})
  file(CHMOD_RECURSE ${KF_SRC}
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ
    DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
file(GLOB_RECURSE KF_STAGED LIST_DIRECTORIES false RELATIVE ${KF_STAGE} ${KF_STAGE}/*)
foreach(rel ${KF_STAGED})
  get_filename_component(dir ${KF_SRC}/${rel} DIRECTORY)
  file(MAKE_DIRECTORY ${dir})
  file(COPY_FILE ${KF_STAGE}/${rel} ${KF_SRC}/${rel} ONLY_IF_DIFFERENT)
endforeach()
# what decomp_src/ has and the staging copy does not (a patch's removed file, an old folder) goes
file(GLOB_RECURSE KF_OLD LIST_DIRECTORIES true RELATIVE ${KF_SRC} ${KF_SRC}/*)
foreach(rel ${KF_OLD})
  if(NOT EXISTS ${KF_STAGE}/${rel})
    file(REMOVE_RECURSE ${KF_SRC}/${rel})
  endif()
endforeach()
file(REMOVE_RECURSE ${KF_STAGE})
# decomp_src/ is generated: compiler errors point into it, but an edit there is lost at the next
# configure. Its files are read-only so that the edit fails instead; edit work/ (tools/patches.py).
file(CHMOD_RECURSE ${KF_SRC}
  FILE_PERMISSIONS OWNER_READ GROUP_READ WORLD_READ
  DIRECTORY_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

# Verdite's headers first, then the patched decomp's: kf/platform and kf/renderer exist only in port/.
set(KF_INCLUDES ${CMAKE_SOURCE_DIR}/port/include ${KF_SRC}/include)

# ---- Libraries ----------------------------------------------------------------------------------
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# The Nix dev shell supplies a delta derived from the translator's PPF release; otherwise a local
# english-v1.kfdelta in the build directory is used. The script is the decomp's, used as is.
if(DEFINED ENV{KF_ENGLISH_PATCH} AND NOT "$ENV{KF_ENGLISH_PATCH}" STREQUAL "")
  set(KF_ENGLISH_PATCH_DEFAULT "$ENV{KF_ENGLISH_PATCH}")
else()
  set(KF_ENGLISH_PATCH_DEFAULT "${CMAKE_BINARY_DIR}/english-v1.kfdelta")
endif()
set(KF_ENGLISH_PATCH "${KF_ENGLISH_PATCH_DEFAULT}" CACHE FILEPATH
    "English translation delta to embed (optional; never distribute builds containing it)")
# Checked on every build, so a payload added after configuring is picked up;
# the generated file is rewritten only when its content changes.
add_custom_target(kf_english_patch
  COMMAND ${Python3_EXECUTABLE} "${KF_DECOMP}/scripts/english_patch.py" embed
    --input "${KF_ENGLISH_PATCH}" --optional --output "${CMAKE_BINARY_DIR}/generated/english_patch.inc"
  BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/english_patch.inc" VERBATIM)

# The game: every source in the copied game, cutscene and lib folders but the three kf_support takes.
file(GLOB KF_GAME_SOURCES CONFIGURE_DEPENDS
  ${KF_SRC}/src/game/*.cpp ${KF_SRC}/src/cutscene/*.cpp ${KF_SRC}/src/lib/*.cpp)
list(REMOVE_ITEM KF_GAME_SOURCES ${KF_SRC}/src/lib/fixed_math.cpp ${KF_SRC}/src/lib/random.cpp
  ${KF_SRC}/src/lib/resource_decode.cpp)
set_source_files_properties(${KF_GAME_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-Wall;-Wextra;-fwrapv;-fno-strict-aliasing")

add_library(kf_support STATIC port/platform/files.cpp port/platform/disc.cpp port/platform/assets.cpp
  port/platform/language.cpp port/platform/translation.cpp port/platform/translation_ppf.cpp
  port/platform/controls.cpp port/renderer/tim.cpp ${KF_SRC}/src/audio/codec.cpp ${KF_SRC}/src/lib/fixed_math.cpp
  ${KF_SRC}/src/lib/random.cpp ${KF_SRC}/src/lib/resource_decode.cpp)
target_sources(kf_support PRIVATE "${CMAKE_BINARY_DIR}/generated/english_patch.inc")
target_include_directories(kf_support PUBLIC ${KF_INCLUDES} PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(kf_support kf_english_patch)
target_compile_options(kf_support PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(WIN32)
  # Windows: the SDL3 MinGW development package is downloaded unless SDL3 is
  # already on CMAKE_PREFIX_PATH.
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    set(KF_SDL3_VERSION 3.4.16)
    set(KF_SDL3_ROOT "${CMAKE_BINARY_DIR}/SDL3-${KF_SDL3_VERSION}")
    if(NOT EXISTS "${KF_SDL3_ROOT}")
      set(KF_SDL3_ARCHIVE "${CMAKE_BINARY_DIR}/SDL3-devel-${KF_SDL3_VERSION}-mingw.tar.gz")
      message(STATUS "Downloading SDL3 ${KF_SDL3_VERSION} (MinGW)")
      file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-${KF_SDL3_VERSION}/SDL3-devel-${KF_SDL3_VERSION}-mingw.tar.gz"
        "${KF_SDL3_ARCHIVE}" STATUS KF_SDL3_STATUS TLS_VERIFY ON)
      list(GET KF_SDL3_STATUS 0 KF_SDL3_CODE)
      if(NOT KF_SDL3_CODE EQUAL 0)
        message(FATAL_ERROR "Cannot download SDL3 (${KF_SDL3_STATUS}). Set CMAKE_PREFIX_PATH to an SDL3 MinGW package instead.")
      endif()
      file(ARCHIVE_EXTRACT INPUT "${KF_SDL3_ARCHIVE}" DESTINATION "${CMAKE_BINARY_DIR}")
    endif()
    find_package(SDL3 CONFIG REQUIRED PATHS "${KF_SDL3_ROOT}/x86_64-w64-mingw32" NO_DEFAULT_PATH)
  endif()
  # Windows has no system libGLESv2: GLES 3.0 is resolved through SDL (third_party/glshim).
  add_library(kf_glshim STATIC third_party/glshim/glshim.c)
  target_include_directories(kf_glshim PUBLIC third_party/gles)
  target_compile_definitions(kf_glshim PUBLIC KHRONOS_STATIC)
  target_link_libraries(kf_glshim PUBLIC SDL3::SDL3)
  set(KF_GLES_LIBRARY kf_glshim)
else()
  find_package(SDL3 CONFIG REQUIRED)
  find_library(KF_GLES_LIBRARY GLESv2 REQUIRED)
endif()

# PC extensions: overlay (Dear ImGui), console, settings and Lua mods.
set(KF_IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
file(GLOB KF_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/l*.c)
list(FILTER KF_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|ltests|onelua)\\.c$")
add_library(kf_ext STATIC
  port/ext/console.cpp port/ext/settings.cpp port/ext/mods.cpp port/ext/overlay.cpp port/ext/automap.cpp
  port/ext/item_text.cpp
  ${KF_IMGUI}/imgui.cpp ${KF_IMGUI}/imgui_draw.cpp ${KF_IMGUI}/imgui_tables.cpp ${KF_IMGUI}/imgui_widgets.cpp
  ${KF_IMGUI}/backends/imgui_impl_sdl3.cpp ${KF_IMGUI}/backends/imgui_impl_opengl3.cpp
  ${KF_LUA_SOURCES} third_party/cjson/cJSON.c)
target_include_directories(kf_ext PUBLIC ${KF_INCLUDES} ${KF_IMGUI} third_party/lua third_party/cjson)
target_compile_definitions(kf_ext PUBLIC KF_EXT=1 PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(kf_ext PUBLIC SDL3::SDL3 ${KF_GLES_LIBRARY})

# libverdite: nothing from the game on its include path.
find_package(Threads REQUIRED)
add_library(verdite STATIC libverdite/src/agent.cpp)
target_include_directories(verdite PUBLIC libverdite/include)
target_link_libraries(verdite PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(verdite PUBLIC ws2_32)
endif()
target_compile_options(verdite PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(kf_host STATIC port/platform/host.cpp port/platform/agent.cpp port/platform/saves.cpp port/platform/language_runtime.cpp
  port/platform/automap.cpp ${KF_SRC}/src/audio/sound.cpp port/renderer/renderer.cpp
  port/renderer/textures.cpp port/renderer/lighting.cpp port/renderer/projection.cpp)
target_include_directories(kf_host PUBLIC ${KF_INCLUDES})
target_link_libraries(kf_host PUBLIC kf_support SDL3::SDL3 kf_ext verdite)
if(WIN32)
  target_link_libraries(kf_host PUBLIC kf_glshim)
endif()
target_compile_options(kf_host PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(kings-field port/platform/entry.cpp ${KF_GAME_SOURCES})
target_link_libraries(kings-field PRIVATE kf_host ${KF_GLES_LIBRARY})
target_compile_options(kings-field PRIVATE -Wall -Wextra)
if(WIN32)
  # Static C++ runtime: the game folder needs only SDL3.dll beside the exe.
  target_link_options(kings-field PRIVATE -static)
  # A windowed app (no console window); output goes to kf.log and the F1 console.
  option(KF_WINDOWED "Build without a console window" ON)
  if(KF_WINDOWED)
    target_link_options(kings-field PRIVATE -mwindows)
  endif()
  add_custom_command(TARGET kings-field POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:kings-field>)
endif()
install(TARGETS kings-field RUNTIME DESTINATION bin)
