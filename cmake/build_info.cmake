# Run on every build (kf_build_info): writes OUTPUT with the commit the build is from, "+edits"
# when the tree differs from it, for crash reports. Rewritten only when it changes.
execute_process(COMMAND git -C "${SOURCE}" describe --always --dirty=+edits --abbrev=12
  OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR commit STREQUAL "")
  set(commit "unknown")
endif()
file(WRITE "${OUTPUT}.new" "#define KF_BUILD_COMMIT \"${commit}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT}.new" "${OUTPUT}")
file(REMOVE "${OUTPUT}.new")
