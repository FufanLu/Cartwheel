# Shared by the *-asan / *-tsan overlay triplets.
#
# Why dependencies must be sanitized too: protobuf and abseil change class
# layout and code paths under sanitizers (PROTOBUF_TSAN_DECLARE_MEMBER adds a
# field; ABSL_HAVE_ADDRESS_SANITIZER switches arena/poisoning code). Linking
# instrumented Cartwheel code against uninstrumented libprotobuf is an ODR
# violation that crashes under ASan and floods TSan with false races.
#
# Deps get ASan (no UBSan) or TSan only. UBSan does not affect ABI, and
# keeping it to first-party code avoids noise from third-party UB.
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# Release-only dependencies (see release-only.cmake).
set(VCPKG_BUILD_TYPE release)
set(_cw_flags "-fsanitize=${CW_TRIPLET_SANITIZER} -fno-omit-frame-pointer -g")
set(VCPKG_C_FLAGS "${_cw_flags}")
set(VCPKG_CXX_FLAGS "${_cw_flags}")
set(VCPKG_LINKER_FLAGS "-fsanitize=${CW_TRIPLET_SANITIZER}")
# Build-time tools (protoc, grpc_cpp_plugin) come from the host triplet, so
# they are not instrumented. Leak checks are disabled in case a port runs a
# freshly built test binary.
set(VCPKG_ENV_PASSTHROUGH ASAN_OPTIONS TSAN_OPTIONS)
