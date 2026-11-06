# cw-tour: runs the real primitives and prints the traces used in
# docs/illustrated/. Regenerate the docs with `build/dev/src/tools/cw-tour`.
add_executable(cw-tour cw_tour.cc)
cartwheel_target_defaults(cw-tour)
target_link_libraries(cw-tour PRIVATE cartwheel::common CLI11::CLI11)
