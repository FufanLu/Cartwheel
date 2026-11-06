add_library(cartwheel_cart
  cart_service.cc
  cart_store.cc
  or_set.cc)
add_library(cartwheel::cart ALIAS cartwheel_cart)
cartwheel_target_defaults(cartwheel_cart)
target_link_libraries(cartwheel_cart
  PUBLIC
    cartwheel::common
    cartwheel::kv
    cartwheel_cart_proto
    gRPC::grpc++)

add_executable(cart-svc cart_main.cc)
cartwheel_target_defaults(cart-svc)
target_link_libraries(cart-svc PRIVATE cartwheel::cart cartwheel::client cartwheel_grpc_reflection CLI11::CLI11)
