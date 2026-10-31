# verdite-online, the client library (online/README.md): knows neither libverdite nor the game, so
# it can move out as a subtree. libdatachannel is built from its source with its submodules
# (KF_LIBDATACHANNEL_SOURCE, from the nix shell): libjuice for ICE, usrsctp, OpenSSL for DTLS.
# A plain variable, so libdatachannel's option() puts no global BUILD_SHARED_LIBS=ON in the cache
# (libchdr, configured before it, would then build shared on the next configure).
set(BUILD_SHARED_LIBS OFF)
set(NO_EXAMPLES ON CACHE BOOL "libdatachannel's examples" FORCE)
set(NO_TESTS ON CACHE BOOL "libdatachannel's tests" FORCE)
set(NO_MEDIA ON CACHE BOOL "libdatachannel's media transport: data channels only" FORCE)
add_subdirectory($ENV{KF_LIBDATACHANNEL_SOURCE} libdatachannel EXCLUDE_FROM_ALL SYSTEM)
# libdatachannel needs RTTI, which the rest of the build turns off
target_compile_options(datachannel-static PRIVATE -frtti)

find_package(OpenSSL REQUIRED)

add_library(verdite_online STATIC src/board.cpp src/floor_frames.cpp src/handover.cpp src/record.cpp
  src/session.cpp src/signaling.cpp ${CMAKE_SOURCE_DIR}/third_party/cjson/cJSON.c)
target_include_directories(verdite_online PUBLIC include
  PRIVATE src ${CMAKE_SOURCE_DIR}/third_party/cjson)
target_link_libraries(verdite_online PRIVATE LibDataChannel::LibDataChannelStatic OpenSSL::Crypto)
target_compile_options(verdite_online PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>)

# vo_pair: two sessions through a running service (online/README.md)
add_executable(vo_pair tests/pair.cpp)
target_link_libraries(vo_pair PRIVATE verdite_online)
target_compile_options(vo_pair PRIVATE -Wall -Wextra -Wpedantic -Werror)

# vo_board_trio: three boards on one floor of a running service (online/README.md)
add_executable(vo_board_trio tests/board_trio.cpp)
target_link_libraries(vo_board_trio PRIVATE verdite_online)
target_compile_options(vo_board_trio PRIVATE -Wall -Wextra -Wpedantic -Werror)

# libFuzzer: the service's frames and the mesh's packets (KF_LIBFUZZER, the fuzz preset)
if(KF_LIBFUZZER)
  add_executable(fuzz_online_frames tests/fuzz_frames.cpp src/floor_frames.cpp src/record.cpp src/signaling.cpp
    ${CMAKE_SOURCE_DIR}/third_party/cjson/cJSON.c)
  target_include_directories(fuzz_online_frames PRIVATE src ${CMAKE_SOURCE_DIR}/third_party/cjson)
  target_link_libraries(fuzz_online_frames PRIVATE OpenSSL::Crypto)
  target_link_options(fuzz_online_frames PRIVATE -fsanitize=fuzzer)
  # beside the game's targets, where scripts/fuzz.py --targets looks
  set_target_properties(fuzz_online_frames PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

if(doctest_FOUND)
  add_executable(verdite_online_tests tests/main.cpp tests/floor_frames.cpp tests/handover.cpp tests/record.cpp
    tests/signaling.cpp
    src/floor_frames.cpp src/handover.cpp src/record.cpp src/signaling.cpp
    ${CMAKE_SOURCE_DIR}/third_party/cjson/cJSON.c)
  target_include_directories(verdite_online_tests PRIVATE src ${CMAKE_SOURCE_DIR}/third_party/cjson)
  target_link_libraries(verdite_online_tests PRIVATE doctest::doctest OpenSSL::Crypto)
  target_compile_options(verdite_online_tests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Werror>)
  add_test(NAME verdite_online COMMAND verdite_online_tests)
endif()
