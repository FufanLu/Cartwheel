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
  gtest_discover_tests(${name}
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${ARG_TIMEOUT})
endfunction()

add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(sim)
add_subdirectory(crash)
add_subdirectory(fuzz)
