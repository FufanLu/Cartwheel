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
