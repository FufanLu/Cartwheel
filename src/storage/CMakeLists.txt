add_library(cartwheel_storage
  mem_engine.cc)
add_library(cartwheel::storage ALIAS cartwheel_storage)
cartwheel_target_defaults(cartwheel_storage)
target_link_libraries(cartwheel_storage PUBLIC cartwheel::common)
