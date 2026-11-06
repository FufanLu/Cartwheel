cartwheel_add_test(stack_test
  SOURCES stack_test.cc
  LIBS cartwheel::gateway cartwheel::cart cartwheel::client
  LABELS integration)

cartwheel_add_test(cluster_grpc_test
  SOURCES cluster_grpc_test.cc
  LIBS cartwheel::kv cartwheel::client
  LABELS integration)

# Needs Redis, Postgres and Kafka (scripts/local_infra.sh); skips itself
# when CW_TEST_* are unset.
cartwheel_add_test(deal_infra_test
  SOURCES deal_infra_test.cc
  LIBS cartwheel::deal_infra
  LABELS integration infra
  TIMEOUT 180)
