cmake_minimum_required(VERSION 3.25)
project(kings_field VERSION 0.1.0 LANGUAGES C CXX)
# Verdite1 on the decomp. The layout follows holyland:
#   decomp/       sushi-shi's source port (our fork), a submodule pinned on `port`; never edited
#   patches/      Verdite's edits to its game code, one diff per copied directory (tools/patches.py)
#   port/         the platform layer, renderer and PC extensions: Verdite's own, edited in place
#   libverdite/   the game-agnostic C++ library the Verdite ports share, to become a subtree; knows no game
#   third_party/  imgui, lua, cjson, libchdr
# At configure time the decomp's game code (KF_COPIED) is copied, the patches are applied to the
# copy, and the copy (build/<preset>/decomp_src/) is compiled with port/.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# compile_commands.json, for clang-tidy (scripts/lint.py)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
# On Android the game is a shared library (libmain.so), and every static library goes into it.
if(ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
option(KF_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
# Crash reports name the shipped binary's stack from its own line tables (libverdite/crash.h).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-gline-tables-only)
endif()
if(KF_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
# libstdc++'s checked operator[] and front/back on std::array, std::span and std::vector: an index
# past an array inside a struct stays inside the global, where ASan does not see it (fuzz preset).
option(KF_HARDEN "Check standard container bounds (_GLIBCXX_ASSERTIONS)" OFF)
if(KF_HARDEN)
  add_compile_definitions(_GLIBCXX_ASSERTIONS)
endif()
# libFuzzer targets (fuzz preset): everything carries its coverage, the targets link the fuzzer.
option(KF_LIBFUZZER "Build the libFuzzer targets (fuzz_*)" OFF)
if(KF_LIBFUZZER)
  add_compile_options(-fsanitize=fuzzer-no-link)
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
  port/platform/user_folder.cpp
  port/platform/language.cpp port/platform/translation.cpp port/platform/translation_ppf.cpp
  port/platform/controls.cpp port/renderer/tim.cpp ${KF_SRC}/src/audio/codec.cpp ${KF_SRC}/src/lib/fixed_math.cpp
  ${KF_SRC}/src/lib/random.cpp ${KF_SRC}/src/lib/resource_decode.cpp)
target_sources(kf_support PRIVATE "${CMAKE_BINARY_DIR}/generated/english_patch.inc")
target_include_directories(kf_support PUBLIC ${KF_INCLUDES} PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(kf_support kf_english_patch)
target_compile_options(kf_support PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(kf_support PRIVATE verdite SDL3::SDL3)

# Android builds its libraries from the sources the android nix shell names (KF_*_SOURCE), the same
# versions the desktop links; they would not be found as packages under the NDK's sysroot.
if(ANDROID)
  foreach(source KF_SDL3_SOURCE KF_VULKAN_HEADERS_SOURCE KF_VOLK_SOURCE KF_VMA_SOURCE KF_VK_BOOTSTRAP_SOURCE
      KF_CPPTRACE_SOURCE)
    if(NOT DEFINED ENV{${source}})
      message(FATAL_ERROR "${source} is not set: configure Android in the android shell (nix develop .#android)")
    endif()
  endforeach()
endif()

if(ANDROID)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  add_subdirectory($ENV{KF_SDL3_SOURCE} sdl3 EXCLUDE_FROM_ALL SYSTEM)
elseif(WIN32)
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
else()
  find_package(SDL3 CONFIG REQUIRED)
endif()

# PC extensions: overlay (Dear ImGui), console, settings and Lua mods.
set(KF_IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
file(GLOB KF_LUA_SOURCES ${CMAKE_SOURCE_DIR}/third_party/lua/l*.c)
list(FILTER KF_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|ltests|onelua)\\.c$")
# Dear ImGui's core and its SDL platform backend; each renderer brings its own renderer backend.
add_library(kf_imgui STATIC
  ${KF_IMGUI}/imgui.cpp ${KF_IMGUI}/imgui_draw.cpp ${KF_IMGUI}/imgui_tables.cpp ${KF_IMGUI}/imgui_widgets.cpp
  ${KF_IMGUI}/backends/imgui_impl_sdl3.cpp)
target_include_directories(kf_imgui PUBLIC ${KF_IMGUI} ${KF_IMGUI}/backends)
target_link_libraries(kf_imgui PUBLIC SDL3::SDL3)
add_library(kf_ext STATIC
  port/ext/console.cpp port/ext/settings.cpp port/ext/mods.cpp port/ext/overlay.cpp port/ext/automap.cpp
  port/ext/item_text.cpp port/ext/discord.cpp ${KF_LUA_SOURCES} third_party/cjson/cJSON.c)
target_include_directories(kf_ext PUBLIC ${KF_INCLUDES} ${KF_IMGUI} third_party/lua third_party/cjson)
target_compile_definitions(kf_ext PUBLIC KF_EXT=1)
target_link_libraries(kf_ext PUBLIC kf_imgui SDL3::SDL3 verdite)

# libverdite: nothing from the game on its include path.
find_package(Threads REQUIRED)
# cpptrace names a crash report's stack (libverdite/src/crash*.cpp). The nix shell has it; Windows
# builds it from its release, with libdwarf, to read the line tables the exe carries.
if(ANDROID)
  # No libdwarf on Android: symbols from the dynamic linker, the stack from the unwinder.
  set(CPPTRACE_GET_SYMBOLS_WITH_LIBDL ON CACHE BOOL "" FORCE)
  set(CPPTRACE_UNWIND_WITH_UNWIND ON CACHE BOOL "" FORCE)
  set(CPPTRACE_DEMANGLE_WITH_CXXABI ON CACHE BOOL "" FORCE)
  add_subdirectory($ENV{KF_CPPTRACE_SOURCE} cpptrace EXCLUDE_FROM_ALL SYSTEM)
  # It names exception types with typeid; the rest of the build has no RTTI.
  target_compile_options(cpptrace-lib PRIVATE -frtti)
else()
  find_package(cpptrace CONFIG QUIET)
endif()
if(NOT ANDROID AND NOT cpptrace_FOUND)
  include(FetchContent)
  FetchContent_Declare(cpptrace GIT_REPOSITORY https://github.com/jeremy-rifkin/cpptrace.git
    GIT_TAG v1.0.4 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(cpptrace)
endif()
# libchdr reads CHD disc images (libverdite/src/disc_image.cpp): rtissera/libchdr 47d79415
# (2026-10-11), vendored unchanged without its tests, docs and contrib, plus the LICENSE its zstd
# copy left out; it carries its LZMA, miniz and zstd.
set(CHDR_WANT_TESTS OFF CACHE BOOL "libchdr's own tests" FORCE)
# Where cpptrace already links the system's zstd (the nix shell's libdwarf does), libchdr uses it
# too, through a `zstd` target it then does not build: its own copy's symbols would otherwise
# override the shared library's for libdwarf.
if(TARGET zstd::libzstd_shared)
  add_library(zstd INTERFACE)
  target_link_libraries(zstd INTERFACE zstd::libzstd_shared)
endif()
add_subdirectory(third_party/libchdr EXCLUDE_FROM_ALL SYSTEM)
if(TARGET zstd::libzstd_shared)
  target_compile_definitions(chdr-static PRIVATE CHDR_SYSTEM_ZSTD)
endif()
add_library(verdite STATIC libverdite/src/agent.cpp libverdite/src/crash.cpp libverdite/src/crash_helper.cpp
  libverdite/src/disc_image.cpp libverdite/src/discord.cpp libverdite/src/fuzz.cpp libverdite/src/memory_card.cpp
  libverdite/src/menu_pointer.cpp libverdite/src/places.cpp libverdite/src/state_trace.cpp libverdite/src/stick.cpp)
target_include_directories(verdite PUBLIC libverdite/include)
target_link_libraries(verdite PUBLIC Threads::Threads PRIVATE cpptrace::cpptrace chdr-static)
if(WIN32)
  target_link_libraries(verdite PUBLIC ws2_32)
endif()
target_compile_options(verdite PRIVATE -Wall -Wextra -Wpedantic -Werror)

# libverdite's tests, built when doctest is installed (the nix shell has it): ctest --preset linux
find_package(doctest CONFIG QUIET)
if(doctest_FOUND)
  enable_testing()
  add_executable(verdite_tests libverdite/tests/main.cpp libverdite/tests/agent.cpp libverdite/tests/crash.cpp
    libverdite/tests/disc_image.cpp libverdite/tests/discord.cpp libverdite/tests/fuzz.cpp libverdite/tests/memory_card.cpp
    libverdite/tests/menu_pointer.cpp libverdite/tests/places.cpp libverdite/tests/state_trace.cpp libverdite/tests/stick.cpp)
  target_link_libraries(verdite_tests PRIVATE verdite doctest::doctest)
  target_compile_definitions(verdite_tests PRIVATE VERDITE_TEST_DATA="${CMAKE_SOURCE_DIR}/libverdite/tests/data")
  target_compile_options(verdite_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME verdite COMMAND verdite_tests)
endif()

# The renderer frontend's API-free parts: batching, texture layers, smoothing. Tested on their own.
add_library(kf_frontend STATIC port/renderer/frame_batch.cpp port/renderer/texture_layers.cpp
  port/renderer/view_smoothing.cpp port/renderer/view_rotation.cpp port/renderer/vertex_carry.cpp port/renderer/model_transform.cpp
  port/renderer/lighting.cpp port/renderer/projection.cpp port/renderer/texture_mips.cpp port/renderer/light_sets.cpp)
target_link_libraries(kf_frontend PUBLIC kf_support)
target_compile_options(kf_frontend PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The Vulkan backend: Vulkan 1.1 core, through volk, VMA and vk-bootstrap. Its GLSL shaders are
# compiled to SPIR-V and embedded as arrays (generated/shaders/<name>.h).
if(ANDROID)
  add_subdirectory($ENV{KF_VULKAN_HEADERS_SOURCE} vulkan-headers EXCLUDE_FROM_ALL SYSTEM)
  set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
  add_subdirectory($ENV{KF_VOLK_SOURCE} volk EXCLUDE_FROM_ALL SYSTEM)
  target_link_libraries(volk PUBLIC Vulkan::Headers)
  add_subdirectory($ENV{KF_VMA_SOURCE} vma EXCLUDE_FROM_ALL SYSTEM)
  set(VK_BOOTSTRAP_TEST OFF CACHE BOOL "" FORCE)
  set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)
  set(VK_BOOTSTRAP_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
  add_subdirectory($ENV{KF_VK_BOOTSTRAP_SOURCE} vk-bootstrap EXCLUDE_FROM_ALL SYSTEM)
else()
  find_package(VulkanHeaders CONFIG REQUIRED)
  find_package(volk CONFIG REQUIRED)
  find_package(VulkanMemoryAllocator CONFIG REQUIRED)
  find_package(vk-bootstrap CONFIG REQUIRED)
endif()
find_program(KF_GLSLANG glslangValidator REQUIRED)
set(KF_SHADER_DIR ${CMAKE_SOURCE_DIR}/port/renderer/vulkan/shaders)
set(KF_SHADER_OUT ${CMAKE_BINARY_DIR}/generated/shaders)
set(KF_SHADER_HEADERS)
foreach(shader world.vert world.frag surface.frag fullscreen.vert downsample.frag occlusion.frag
    occlusion_blur.frag)
  string(REPLACE "." "_" symbol ${shader})
  add_custom_command(OUTPUT ${KF_SHADER_OUT}/${symbol}.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KF_SHADER_OUT}
    COMMAND ${KF_GLSLANG} -V --target-env vulkan1.1 --vn ${symbol} -o ${KF_SHADER_OUT}/${symbol}.h ${KF_SHADER_DIR}/${shader}
    DEPENDS ${KF_SHADER_DIR}/${shader} ${KF_SHADER_DIR}/draw.glsl VERBATIM)
  list(APPEND KF_SHADER_HEADERS ${KF_SHADER_OUT}/${symbol}.h)
endforeach()
# third-party code: built with its own warnings, not ours
add_library(kf_vulkan_libraries STATIC port/renderer/vulkan/vulkan_libraries.cpp
  ${KF_IMGUI}/backends/imgui_impl_vulkan.cpp)
target_compile_definitions(kf_vulkan_libraries PUBLIC VK_NO_PROTOTYPES VMA_STATIC_VULKAN_FUNCTIONS=0
  VMA_DYNAMIC_VULKAN_FUNCTIONS=1 IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(kf_vulkan_libraries PUBLIC Vulkan::Headers volk::volk GPUOpen::VulkanMemoryAllocator
  vk-bootstrap::vk-bootstrap kf_imgui)
add_library(kf_vulkan STATIC port/renderer/vulkan/vulkan_backend.cpp port/renderer/vulkan/vulkan_device.cpp
  port/renderer/vulkan/vulkan_ambient_occlusion.cpp port/renderer/vulkan/vulkan_downsample.cpp
  port/renderer/vulkan/vulkan_pipelines.cpp port/renderer/vulkan/vulkan_render_pass.cpp port/renderer/vulkan/vulkan_surfaces.cpp
  port/renderer/vulkan/vulkan_resources.cpp port/renderer/vulkan/vulkan_swapchain.cpp ${KF_SHADER_HEADERS})
target_include_directories(kf_vulkan PUBLIC ${KF_INCLUDES} PRIVATE ${KF_SHADER_OUT})
target_link_libraries(kf_vulkan PUBLIC kf_vulkan_libraries kf_frontend SDL3::SDL3)
target_compile_options(kf_vulkan PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(kf_host STATIC port/platform/host.cpp port/platform/agent.cpp port/platform/fuzz.cpp port/platform/saves.cpp port/platform/language_runtime.cpp
  port/platform/automap.cpp port/platform/screen.cpp port/platform/analog.cpp ${KF_SRC}/src/audio/sound.cpp port/renderer/textures.cpp port/renderer/frontend.cpp port/renderer/null_backend.cpp)
target_sources(kf_host PRIVATE port/platform/crash.cpp port/platform/logcat.cpp)
target_include_directories(kf_host PUBLIC ${KF_INCLUDES} PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(kf_host PUBLIC kf_support kf_frontend kf_vulkan SDL3::SDL3 kf_ext verdite)
if(ANDROID)
  target_link_libraries(kf_host PUBLIC log)
endif()
# The build a crash report names: the version, the commit (checked on every build) and the type.
add_custom_target(kf_build_info
  COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUTPUT=${CMAKE_BINARY_DIR}/generated/build_info.h
    -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/build_info.h" VERBATIM)
add_dependencies(kf_host kf_build_info)
target_compile_definitions(kf_host PRIVATE KF_VERSION="${PROJECT_VERSION}" KF_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_compile_options(kf_host PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(ANDROID)
  # SDL's Java activity loads libSDL3.so and then libmain.so, and calls its SDL_main.
  add_library(verdite1 SHARED port/platform/entry.cpp ${KF_GAME_SOURCES})
  set_target_properties(verdite1 PROPERTIES OUTPUT_NAME main)
else()
  add_executable(verdite1 port/platform/entry.cpp ${KF_GAME_SOURCES})
endif()
target_link_libraries(verdite1 PRIVATE kf_host)
target_compile_options(verdite1 PRIVATE -Wall -Wextra)
if(ANDROID)
  # The libraries the APK carries, where android/app/build.gradle picks them up: jni/<abi>/.
  set(KF_JNI_DIR ${CMAKE_BINARY_DIR}/jni/${ANDROID_ABI})
  # The NDK keeps each ABI's C++ runtime under its target triple.
  set(KF_TRIPLE_arm64-v8a aarch64-linux-android)
  set(KF_TRIPLE_x86_64 x86_64-linux-android)
  set(KF_CXX_RUNTIME ${ANDROID_TOOLCHAIN_ROOT}/sysroot/usr/lib/${KF_TRIPLE_${ANDROID_ABI}}/libc++_shared.so)
  add_custom_command(TARGET verdite1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KF_JNI_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:verdite1> $<TARGET_FILE:SDL3::SDL3-shared>
      ${KF_CXX_RUNTIME} ${KF_JNI_DIR}
    VERBATIM)
endif()
if(WIN32)
  # Static C++ runtime: the game folder needs only SDL3.dll beside the exe.
  target_link_options(verdite1 PRIVATE -static)
  # A windowed app (no console window); output goes to kf.log and the F1 console.
  option(KF_WINDOWED "Build without a console window" ON)
  if(KF_WINDOWED)
    target_link_options(verdite1 PRIVATE -mwindows)
  endif()
  add_custom_command(TARGET verdite1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL3::SDL3> $<TARGET_FILE_DIR:verdite1>)
endif()
install(TARGETS verdite1 RUNTIME DESTINATION bin)

# libFuzzer targets, for what comes from outside the game (docs/FUZZING_PLAN.md; run them with
# python3 scripts/fuzz.py --targets). fuzz_codecs is the decomp's own, built against our patches.
if(KF_LIBFUZZER)
  add_executable(fuzz_memory_card libverdite/tests/fuzz/memory_card.cpp)
  target_link_libraries(fuzz_memory_card PRIVATE verdite)
  add_executable(fuzz_disc_image libverdite/tests/fuzz/disc_image.cpp)
  target_link_libraries(fuzz_disc_image PRIVATE verdite)
  add_executable(fuzz_codecs ${KF_DECOMP}/tests/fuzz_codecs.cpp)
  target_link_libraries(fuzz_codecs PRIVATE kf_support verdite SDL3::SDL3)
  add_executable(fuzz_save_card port/tests/fuzz/save_card.cpp ${KF_GAME_SOURCES})
  target_link_libraries(fuzz_save_card PRIVATE kf_host)
  foreach(target fuzz_memory_card fuzz_disc_image fuzz_codecs fuzz_save_card)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
  endforeach()
endif()

# port/'s tests: the parts that run without a window or GPU.
if(doctest_FOUND)
  add_executable(kf_port_tests port/tests/main.cpp port/tests/renderer.cpp)
  target_link_libraries(kf_port_tests PRIVATE kf_frontend doctest::doctest)
  target_compile_options(kf_port_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME port COMMAND kf_port_tests)
  # The Vulkan backend on a real GPU, offscreen, validation layers on: any validation error fails it.
  # It loads the host's Vulkan loader and drivers, as scripts/run-linux.sh does; 77: no device.
  add_executable(kf_vulkan_tests port/tests/vulkan.cpp)
  target_link_libraries(kf_vulkan_tests PRIVATE kf_vulkan kf_frontend kf_support doctest::doctest)
  target_include_directories(kf_vulkan_tests PRIVATE port/renderer/vulkan)
  target_compile_options(kf_vulkan_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  set(KF_HOST_LIBRARY_PATH "/usr/lib64" CACHE PATH "Where the host's libvulkan.so.1 is, for the Vulkan tests")
  foreach(device gpu llvmpipe)
    add_test(NAME vulkan_${device} COMMAND kf_vulkan_tests)
    # host layers that insert themselves (frame generators, overlays) stay out of the test
    # No leak detection here (code-style.md): the host drivers' own allocations are reported from
    # modules already unloaded. Vulkan objects we leak are validation errors, which fail the test.
    set(environment KF_VK_VALIDATION=1 LD_LIBRARY_PATH=${KF_HOST_LIBRARY_PATH} VK_LOADER_LAYERS_DISABLE=~implicit~
      ASAN_OPTIONS=detect_leaks=0)
    if(device STREQUAL llvmpipe)
      list(APPEND environment KF_VK_DEVICE=llvmpipe)
    endif()
    set_tests_properties(vulkan_${device} PROPERTIES ENVIRONMENT "${environment}" SKIP_RETURN_CODE 77
      FAIL_REGULAR_EXPRESSION "\\[vulkan\\] error")
  endforeach()
endif()
