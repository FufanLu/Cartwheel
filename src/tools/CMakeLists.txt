# cw-tour: runs the real primitives and prints the traces used in
# docs/illustrated/. Regenerate the docs with `build/dev/src/tools/cw-tour`.
add_executable(cw-tour cw_tour.cc)
cartwheel_target_defaults(cw-tour)
target_link_libraries(cw-tour PRIVATE cartwheel::common CLI11::CLI11)

# cw-trace: DVV + cart OR-Set traces for docs/illustrated/01.
add_executable(cw-trace cw_trace.cc)
cartwheel_target_defaults(cw-trace)
target_link_libraries(cw-trace PRIVATE cartwheel::cart cartwheel::kv)

add_executable(wheelctl wheelctl.cc)
cartwheel_target_defaults(wheelctl)
target_link_libraries(wheelctl PRIVATE cartwheel::kv CLI11::CLI11)
