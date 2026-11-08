# iOS dependencies must be compiled for iOS, never taken from Homebrew's macOS libraries.
enable_language(OBJC OBJCXX)
set(CMAKE_XCODE_GENERATE_SCHEME ON)
include(FetchContent)
set(KF_IOS_TEAM "" CACHE STRING "Apple development team used to install on a device")
set(KF_IOS_BUNDLE_ID "io.github.voicedrew11.verdite1" CACHE STRING "iOS application bundle identifier")
set(KF_IOS_DEPS "${CMAKE_SOURCE_DIR}/build/ios-deps" CACHE PATH "Downloaded iOS dependencies")
file(MAKE_DIRECTORY "${KF_IOS_DEPS}")
set(KF_MVK_ROOT "${KF_IOS_DEPS}/MoltenVK")
if(NOT EXISTS "${KF_MVK_ROOT}/MoltenVK/dynamic/MoltenVK.xcframework")
  file(DOWNLOAD "https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar"
    "${KF_IOS_DEPS}/MoltenVK-ios.tar"
    EXPECTED_HASH SHA256=b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1 TLS_VERIFY ON)
  file(ARCHIVE_EXTRACT INPUT "${KF_IOS_DEPS}/MoltenVK-ios.tar" DESTINATION "${KF_IOS_DEPS}")
endif()
set(KF_MOLTENVK_FRAMEWORK "${KF_MVK_ROOT}/MoltenVK/dynamic/MoltenVK.xcframework/ios-arm64/MoltenVK.framework")
add_library(Vulkan::Headers INTERFACE IMPORTED)
set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${KF_MVK_ROOT}/MoltenVK/include")
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
FetchContent_Declare(volk SYSTEM
  GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.321.0 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(volk)
target_link_libraries(volk PUBLIC Vulkan::Headers)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 SYSTEM
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
  URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
FetchContent_MakeAvailable(SDL3)
