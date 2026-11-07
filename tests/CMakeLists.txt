# cartwheel_add_test(<name> SOURCES ... [LIBS ...] [LABELS ...])
# One gtest binary per module; ctest sees every TEST() via gtest_discover_tests.
function(cartwheel_add_test name)
  cmake_parse_arguments(ARG "" "TIMEOUT" "SOURCES;LIBS;LABELS" ${ARGN})
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 120)
  endif()
  add_executable(${name} ${ARG_SOURCES})
  cartwheel_target_defaults(${name})
  target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  # Discover at test time, not build time: a sanitized binary on a loaded
  # machine can take longer than any build-time timeout just to list tests.
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 120
    PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${ARG_TIMEOUT})
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(sim)
add_subdirectory(crash)
add_subdirectory(fuzz)
