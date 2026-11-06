add_library(cartwheel_gateway
  cart_routes.cc
  http.cc
  http_server.cc
  io_pool.cc)
add_library(cartwheel::gateway ALIAS cartwheel_gateway)
cartwheel_target_defaults(cartwheel_gateway)
target_link_libraries(cartwheel_gateway
  PUBLIC
    cartwheel::common
    cartwheel::kv
    cartwheel_cart_proto
    Boost::asio
    Boost::beast
    nlohmann_json::nlohmann_json
    gRPC::grpc++)

add_executable(gateway gateway_main.cc)
cartwheel_target_defaults(gateway)
target_link_libraries(gateway PRIVATE cartwheel::gateway CLI11::CLI11)
