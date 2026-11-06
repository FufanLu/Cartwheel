# Fuzz targets. With -DCARTWHEEL_LIBFUZZER=ON (Linux clang), each target links
# libFuzzer for coverage-guided runs:   build/fuzz/tests/fuzz/wal_fuzz -max_total_time=60
# Otherwise each links fuzz_driver_main.cc and runs as a ctest smoke test.
option(CARTWHEEL_LIBFUZZER "Build fuzz targets against libFuzzer (-fsanitize=fuzzer)" OFF)

function(cartwheel_fuzz_target name)
  add_executable(${name} ${name}.cc)
  cartwheel_target_defaults(${name})
  target_link_libraries(${name} PRIVATE cartwheel::storage cartwheel::cluster)
  if(CARTWHEEL_LIBFUZZER)
    target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  else()
    target_sources(${name} PRIVATE fuzz_driver_main.cc)
    add_test(NAME ${name} COMMAND ${name} 20000)
    set_tests_properties(${name} PROPERTIES LABELS fuzz TIMEOUT 300)
  endif()
endfunction()

foreach(t wal_fuzz block_fuzz table_fuzz decoders_fuzz)
  cartwheel_fuzz_target(${t})
endforeach()
