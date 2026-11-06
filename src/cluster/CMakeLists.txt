# Pure distributed-systems logic: no gRPC, no threads, no wall clock (ADR 0003).
add_library(cartwheel_cluster
  membership.cc
  merkle.cc
  ring.cc
  siblings.cc
  vector_clock.cc)
add_library(cartwheel::cluster ALIAS cartwheel_cluster)
cartwheel_target_defaults(cartwheel_cluster)
target_link_libraries(cartwheel_cluster PUBLIC cartwheel::common cartwheel_cluster_proto)
