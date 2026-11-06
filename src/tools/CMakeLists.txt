# cw-tour: runs the real primitives and prints the traces used in
# docs/illustrated/. Regenerate the docs with `build/dev/src/tools/cw-tour`.
add_executable(cw-tour cw_tour.cc)
cartwheel_target_defaults(cw-tour)
target_link_libraries(cw-tour PRIVATE cartwheel::common CLI11::CLI11)

# cw-trace: DVV + cart OR-Set traces for docs/illustrated/01.
add_executable(cw-trace cw_trace.cc)
cartwheel_target_defaults(cw-trace)
target_link_libraries(cw-trace PRIVATE cartwheel::cart cartwheel::kv)

add_executable(wheelctl wheelctl.cc)
cartwheel_target_defaults(wheelctl)
target_link_libraries(wheelctl PRIVATE cartwheel::kv CLI11::CLI11)

# lsm-bench: db_bench-style workloads; RocksDB reference with -DCARTWHEEL_BENCH_ROCKSDB=ON.
add_executable(lsm-bench lsm_bench.cc)
cartwheel_target_defaults(lsm-bench)
target_link_libraries(lsm-bench PRIVATE cartwheel::storage CLI11::CLI11)
if(CARTWHEEL_BENCH_ROCKSDB)
  find_package(RocksDB CONFIG REQUIRED)
  target_link_libraries(lsm-bench PRIVATE RocksDB::rocksdb)
  target_compile_definitions(lsm-bench PRIVATE CARTWHEEL_HAVE_ROCKSDB=1)
endif()

# lsm-tour: byte-level traces of the LSM engine for docs/illustrated/02.
add_executable(lsm-tour lsm_tour.cc)
cartwheel_target_defaults(lsm-tour)
target_link_libraries(lsm-tour PRIVATE cartwheel::storage)

# wheel-bench: YCSB A/B/C against a cluster, per-second timeline.
add_executable(wheel-bench wheel_bench.cc)
cartwheel_target_defaults(wheel-bench)
target_link_libraries(wheel-bench PRIVATE cartwheel::client CLI11::CLI11)

# deal-rush: lightning-deal spike through the gateway (HTTP + WebSocket).
add_executable(deal-rush deal_rush.cc)
cartwheel_target_defaults(deal-rush)
target_link_libraries(deal-rush PRIVATE cartwheel::common Boost::asio Boost::beast
  nlohmann_json::nlohmann_json CLI11::CLI11 cartwheel_pq)
