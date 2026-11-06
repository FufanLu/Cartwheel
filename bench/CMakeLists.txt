add_executable(common_bench common_bench.cc)
cartwheel_target_defaults(common_bench)
target_link_libraries(common_bench PRIVATE cartwheel::common benchmark::benchmark_main)
