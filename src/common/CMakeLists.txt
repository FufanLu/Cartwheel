execute_process(
  COMMAND git rev-parse --short=12 HEAD
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  OUTPUT_VARIABLE CARTWHEEL_GIT_SHA
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(NOT CARTWHEEL_GIT_SHA)
  set(CARTWHEEL_GIT_SHA "unknown")
endif()

set(_cw_gen_dir "${PROJECT_BINARY_DIR}/generated")
configure_file(build_info.h.in "${_cw_gen_dir}/common/build_info.h" @ONLY)

add_library(cartwheel_common
  backoff.cc
  config.cc
  histogram.cc
  logging.cc
  metrics.cc
  signals.cc
  status.cc
  thread_pool.cc
  token_bucket.cc)
add_library(cartwheel::common ALIAS cartwheel_common)
cartwheel_target_defaults(cartwheel_common)

target_include_directories(cartwheel_common PUBLIC
  "${PROJECT_SOURCE_DIR}/src"
  "${_cw_gen_dir}")

# Keep every log macro compiled in; the runtime level decides what prints.
target_compile_definitions(cartwheel_common PUBLIC SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

target_link_libraries(cartwheel_common
  PUBLIC
    fmt::fmt
    spdlog::spdlog
    prometheus-cpp::core
    Threads::Threads
  PRIVATE
    tomlplusplus::tomlplusplus
    prometheus-cpp::pull)
