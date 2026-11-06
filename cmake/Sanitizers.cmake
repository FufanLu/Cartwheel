# Sanitizers apply to the whole Cartwheel tree (not to prebuilt vcpkg deps).
# Because deps are uninstrumented, the asan test preset sets
# detect_container_overflow=0: libc++ annotates std::vector, and vectors that
# cross into uninstrumented code (e.g. spdlog's formatter) report false
# container-overflow errors on macOS.
# ThreadSanitizer and AddressSanitizer are mutually exclusive.

if(CARTWHEEL_SANITIZER STREQUAL "")
  return()
endif()

string(REPLACE "+" ";" _cw_sanitizers "${CARTWHEEL_SANITIZER}")
if("thread" IN_LIST _cw_sanitizers AND "address" IN_LIST _cw_sanitizers)
  message(FATAL_ERROR "thread and address sanitizers cannot be combined")
endif()

set(_cw_flags "")
foreach(_s IN LISTS _cw_sanitizers)
  if(NOT _s MATCHES "^(address|thread|undefined)$")
    message(FATAL_ERROR "Unknown sanitizer '${_s}'")
  endif()
  list(APPEND _cw_flags "-fsanitize=${_s}")
endforeach()

list(APPEND _cw_flags -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_compile_options(${_cw_flags} -g)
add_link_options(${_cw_flags})
message(STATUS "Cartwheel sanitizers: ${CARTWHEEL_SANITIZER}")
