# OpenSSL for Android, for libdatachannel's TLS and DTLS and online/client's signatures. Its build is
# its own Configure and make, not CMake, and find_package needs the result while configuring, so it
# is built here, once per build folder, from the tarball the android nix shell names.
set(KF_OPENSSL_ROOT ${CMAKE_BINARY_DIR}/openssl)
set(KF_OPENSSL_LIBS ${KF_OPENSSL_ROOT}/install/lib)
set(KF_OPENSSL_TARGET_arm64-v8a android-arm64)
set(KF_OPENSSL_TARGET_x86_64 android-x86_64)

function(kf_openssl_run)
  execute_process(COMMAND ${CMAKE_COMMAND} -E env "PATH=${ANDROID_TOOLCHAIN_ROOT}/bin:$ENV{PATH}"
    "ANDROID_NDK_ROOT=${ANDROID_NDK}" ${ARGN}
    WORKING_DIRECTORY ${KF_OPENSSL_ROOT}/source RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "OpenSSL for Android: ${ARGN} failed (${result})")
  endif()
endfunction()

if(NOT EXISTS ${KF_OPENSSL_LIBS}/libssl.a)
  message(STATUS "Building OpenSSL for ${ANDROID_ABI}")
  file(REMOVE_RECURSE ${KF_OPENSSL_ROOT})
  file(ARCHIVE_EXTRACT INPUT $ENV{KF_OPENSSL_SOURCE} DESTINATION ${KF_OPENSSL_ROOT}/unpacked)
  file(GLOB KF_OPENSSL_UNPACKED ${KF_OPENSSL_ROOT}/unpacked/*)
  file(RENAME ${KF_OPENSSL_UNPACKED} ${KF_OPENSSL_ROOT}/source)
  kf_openssl_run(perl Configure ${KF_OPENSSL_TARGET_${ANDROID_ABI}} -D__ANDROID_API__=${ANDROID_PLATFORM_LEVEL}
    no-shared no-tests no-apps no-docs no-engine no-module -fPIC
    --prefix=${KF_OPENSSL_ROOT}/install --libdir=lib)
  cmake_host_system_information(RESULT KF_OPENSSL_JOBS QUERY NUMBER_OF_LOGICAL_CORES)
  kf_openssl_run(make -j${KF_OPENSSL_JOBS} build_libs)
  kf_openssl_run(make install_dev)
endif()

# Named outright: the NDK's toolchain file keeps find_package inside its sysroot.
set(OPENSSL_USE_STATIC_LIBS ON)
set(OPENSSL_INCLUDE_DIR ${KF_OPENSSL_ROOT}/install/include CACHE PATH "" FORCE)
set(OPENSSL_SSL_LIBRARY ${KF_OPENSSL_LIBS}/libssl.a CACHE FILEPATH "" FORCE)
set(OPENSSL_CRYPTO_LIBRARY ${KF_OPENSSL_LIBS}/libcrypto.a CACHE FILEPATH "" FORCE)
# FindOpenSSL lists zlib as a static OpenSSL's need; the NDK's is the one the English patch's unpacker uses.
find_package(ZLIB REQUIRED)
