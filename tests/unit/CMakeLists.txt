cartwheel_add_test(common_test
  SOURCES
    common/backoff_test.cc
    common/coding_test.cc
    common/config_test.cc
    common/histogram_test.cc
    common/logging_test.cc
    common/metrics_test.cc
    common/status_test.cc
    common/thread_pool_test.cc
    common/token_bucket_test.cc
  LIBS cartwheel::common
  LABELS unit)

cartwheel_add_test(cluster_test
  SOURCES
    cluster/siblings_test.cc
    cluster/vector_clock_test.cc
  LIBS cartwheel::cluster
  LABELS unit)

cartwheel_add_test(kv_test
  SOURCES kv/local_store_test.cc
  LIBS cartwheel::kv
  LABELS unit)

cartwheel_add_test(cart_test
  SOURCES cart/cart_test.cc
  LIBS cartwheel::cart
  LABELS unit)

cartwheel_add_test(gateway_test
  SOURCES gateway/router_test.cc
  LIBS cartwheel::gateway
  LABELS unit)

cartwheel_add_test(storage_test
  SOURCES
    storage/db_test.cc
    storage/table_test.cc
    storage/wal_test.cc
  LIBS cartwheel::storage
  LABELS unit)
