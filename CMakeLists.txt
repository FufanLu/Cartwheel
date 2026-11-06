cmake_minimum_required(VERSION 3.25)

# Resolve the vcpkg toolchain before project() so presets work without extra flags.
# Order: explicit -DCMAKE_TOOLCHAIN_FILE > $VCPKG_ROOT > ~/vcpkg.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
  elseif(EXISTS "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "")
  endif()
endif()

# Sanitizer builds link against dependencies built with the same sanitizer
# (see triplets/sanitizer-common.cmake): pick <arch>-<os>-<asan|tsan>.
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_SOURCE_DIR}/triplets" CACHE STRING "")
if(DEFINED CARTWHEEL_SANITIZER AND NOT CARTWHEEL_SANITIZER STREQUAL "" AND NOT DEFINED VCPKG_TARGET_TRIPLET)
  execute_process(COMMAND uname -m OUTPUT_VARIABLE _cw_arch OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND uname -s OUTPUT_VARIABLE _cw_os OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(_cw_arch MATCHES "^(arm64|aarch64)$")
    set(_cw_arch arm64)
  else()
    set(_cw_arch x64)
  endif()
  if(_cw_os STREQUAL "Darwin")
    set(_cw_os osx)
  else()
    set(_cw_os linux)
  endif()
  if(CARTWHEEL_SANITIZER MATCHES "thread")
    set(_cw_san tsan)
  else()
    set(_cw_san asan)
  endif()
  set(VCPKG_TARGET_TRIPLET "${_cw_arch}-${_cw_os}-${_cw_san}" CACHE STRING "")
  message(STATUS "Cartwheel: sanitized dependency triplet ${VCPKG_TARGET_TRIPLET}")
endif()

project(Cartwheel
  VERSION 0.1.0
  DESCRIPTION "Dynamo-style distributed KV store and high-concurrency commerce stack"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CARTWHEEL_BUILD_TESTS "Build unit and simulation tests" ON)
option(CARTWHEEL_BUILD_BENCH "Build micro benchmarks" ON)
option(CARTWHEEL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in Cartwheel targets" ON)
set(CARTWHEEL_SANITIZER "" CACHE STRING
  "Sanitizers to enable: '', address, thread, undefined, or address+undefined")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(tomlplusplus CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(nlohmann_json CONFIG REQUIRED)

# gRPC server reflection (for grpcurl) is optional: vcpkg builds gRPC for the
# sanitizer triplets as a cross-compile, which skips codegen-dependent libs.
add_library(cartwheel_grpc_reflection INTERFACE)
if(TARGET gRPC::grpc++_reflection)
  target_link_libraries(cartwheel_grpc_reflection INTERFACE gRPC::grpc++_reflection)
  target_compile_definitions(cartwheel_grpc_reflection INTERFACE CARTWHEEL_HAVE_GRPC_REFLECTION=1)
endif()

include(ProtoLibrary)
add_subdirectory(proto)
add_subdirectory(src)

if(CARTWHEEL_BUILD_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
  add_subdirectory(tests)
endif()

if(CARTWHEEL_BUILD_BENCH)
  find_package(benchmark CONFIG REQUIRED)
  add_subdirectory(bench)
endif()
