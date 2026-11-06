add_executable(order-worker worker_main.cc)
cartwheel_target_defaults(order-worker)
target_link_libraries(order-worker PRIVATE cartwheel::deal_infra CLI11::CLI11)
