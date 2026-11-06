cartwheel_proto_library(cartwheel_kv_proto
  PROTOS cartwheel/kv/v1/kv.proto
  GRPC)

cartwheel_proto_library(cartwheel_cart_proto
  PROTOS
    cartwheel/cart/v1/cart.proto
    cartwheel/cart/v1/cart_state.proto
  GRPC)
