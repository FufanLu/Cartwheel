cartwheel_add_test(cluster_sim_test
  SOURCES cluster_sim_test.cc
  LIBS cartwheel::kv
  LABELS sim
  # ~100k simulated events per seed; sanitizer builds need headroom.
  TIMEOUT 1800)

# ring-tour: traced walk-through of one key for docs/illustrated/03.
add_executable(ring-tour ring_tour.cc)
cartwheel_target_defaults(ring-tour)
target_link_libraries(ring-tour PRIVATE cartwheel::kv)
