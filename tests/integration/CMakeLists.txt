cartwheel_add_test(stack_test
  SOURCES stack_test.cc
  LIBS cartwheel::gateway cartwheel::cart cartwheel::client
  LABELS integration)

cartwheel_add_test(cluster_grpc_test
  SOURCES cluster_grpc_test.cc
  LIBS cartwheel::kv cartwheel::client
  LABELS integration)
