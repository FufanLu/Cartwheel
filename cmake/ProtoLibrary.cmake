# cartwheel_proto_library(<name> PROTOS <files...> [GRPC] [DEPS <targets...>])
#
# Generates C++ (and optionally gRPC) code for .proto files under proto/ and
# wraps it in a static library. Generated headers are included as
# "cartwheel/<pkg>/v1/<file>.pb.h". Generated code is compiled without the
# first-party warning policy.

set(CARTWHEEL_PROTO_ROOT "${PROJECT_SOURCE_DIR}/proto")
set(CARTWHEEL_PROTO_OUT "${PROJECT_BINARY_DIR}/generated/proto")
file(MAKE_DIRECTORY "${CARTWHEEL_PROTO_OUT}")

function(cartwheel_proto_library name)
  cmake_parse_arguments(ARG "GRPC" "" "PROTOS;DEPS" ${ARGN})
  set(_abs_protos "")
  foreach(_p IN LISTS ARG_PROTOS)
    list(APPEND _abs_protos "${CARTWHEEL_PROTO_ROOT}/${_p}")
  endforeach()

  add_library(${name} STATIC ${_abs_protos})
  target_include_directories(${name} PUBLIC "${CARTWHEEL_PROTO_OUT}")
  target_link_libraries(${name} PUBLIC protobuf::libprotobuf ${ARG_DEPS})
  target_compile_features(${name} PUBLIC cxx_std_20)

  protobuf_generate(
    TARGET ${name}
    LANGUAGE cpp
    IMPORT_DIRS "${CARTWHEEL_PROTO_ROOT}"
    PROTOC_OUT_DIR "${CARTWHEEL_PROTO_OUT}")

  if(ARG_GRPC)
    target_link_libraries(${name} PUBLIC gRPC::grpc++)
    protobuf_generate(
      TARGET ${name}
      LANGUAGE grpc
      GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
      PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
      IMPORT_DIRS "${CARTWHEEL_PROTO_ROOT}"
      PROTOC_OUT_DIR "${CARTWHEEL_PROTO_OUT}")
  endif()
endfunction()
