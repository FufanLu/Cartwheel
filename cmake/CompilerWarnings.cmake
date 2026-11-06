# Warning policy for first-party targets only. Third-party and generated code
# (protobuf/gRPC stubs) link against `cartwheel_options` without warnings.

add_library(cartwheel_warnings INTERFACE)

target_compile_options(cartwheel_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Woverloaded-virtual
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
  -Wmissing-declarations)

if(CARTWHEEL_WARNINGS_AS_ERRORS)
  target_compile_options(cartwheel_warnings INTERFACE -Werror)
endif()

# Every first-party target calls this so the policy lives in one place.
function(cartwheel_target_defaults target)
  target_link_libraries(${target} PRIVATE cartwheel_warnings)
  target_compile_features(${target} PUBLIC cxx_std_20)
endfunction()
