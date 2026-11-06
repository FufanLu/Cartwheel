# Overrides vcpkg's built-in arm64-osx triplet (overlay triplets take precedence).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
include("${CMAKE_CURRENT_LIST_DIR}/release-only.cmake")
