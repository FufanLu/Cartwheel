add_library(cartwheel_client
  kv_client.cc)
add_library(cartwheel::client ALIAS cartwheel_client)
cartwheel_target_defaults(cartwheel_client)
target_link_libraries(cartwheel_client
  PUBLIC cartwheel::common
  PRIVATE cartwheel::kv cartwheel_kv_proto gRPC::grpc++)
