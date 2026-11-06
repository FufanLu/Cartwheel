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
