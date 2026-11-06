add_library(cartwheel_kv
  cluster_node.cc
  grpc_status.cc
  in_process_kv.cc
  kv_service.cc
  local_store.cc
  peer_transport.cc
  timer_scheduler.cc)
add_library(cartwheel::kv ALIAS cartwheel_kv)
cartwheel_target_defaults(cartwheel_kv)
target_link_libraries(cartwheel_kv
  PUBLIC
    cartwheel::common
    cartwheel::cluster
    cartwheel::storage
    cartwheel_kv_proto
    cartwheel_cluster_proto
    gRPC::grpc++)

add_executable(wheel-node wheel_node_main.cc)
cartwheel_target_defaults(wheel-node)
target_link_libraries(wheel-node PRIVATE cartwheel::kv cartwheel_grpc_reflection CLI11::CLI11)
