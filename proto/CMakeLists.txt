cartwheel_proto_library(cartwheel_kv_proto
  PROTOS cartwheel/kv/v1/kv.proto
  GRPC)

cartwheel_proto_library(cartwheel_cart_proto
  PROTOS
    cartwheel/cart/v1/cart.proto
    cartwheel/cart/v1/cart_state.proto
  GRPC)

cartwheel_proto_library(cartwheel_cluster_proto
  PROTOS cartwheel/cluster/v1/cluster.proto
  GRPC)

cartwheel_proto_library(cartwheel_deal_proto
  PROTOS cartwheel/deal/v1/deal.proto
  GRPC)
