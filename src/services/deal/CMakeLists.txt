# Pipeline rules and in-memory ports: no infrastructure clients, so unit tests
# link only this.
add_library(cartwheel_deal_core
  batch_rules.cc
  deal_catalog.cc
  deal_engine.cc
  deal_service.cc
  memory_ports.cc
  order_processor.cc
  redrive.cc)
add_library(cartwheel::deal_core ALIAS cartwheel_deal_core)
cartwheel_target_defaults(cartwheel_deal_core)
target_link_libraries(cartwheel_deal_core
  PUBLIC
    cartwheel::common
    cartwheel::kv
    cartwheel_deal_proto
    gRPC::grpc++
    nlohmann_json::nlohmann_json)

# Redis gate + notifier, Kafka log, Postgres store.
add_library(cartwheel_deal_infra
  kafka_log.cc
  pg_store.cc
  redis_gate.cc)
add_library(cartwheel::deal_infra ALIAS cartwheel_deal_infra)
cartwheel_target_defaults(cartwheel_deal_infra)
target_link_libraries(cartwheel_deal_infra
  PUBLIC
    cartwheel::deal_core
    cartwheel_pq
    redis++::redis++_static
    RdKafka::rdkafka++)

add_executable(deal-svc deal_main.cc)
cartwheel_target_defaults(deal-svc)
target_link_libraries(deal-svc PRIVATE cartwheel::deal_infra cartwheel_grpc_reflection CLI11::CLI11)
