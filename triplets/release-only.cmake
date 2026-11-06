# Default (unsanitized) triplets for Cartwheel: static libraries, and
# dependencies built in release mode only.
#
# Why release-only: we debug Cartwheel's own code, not gRPC's. Building every
# dependency twice (debug + release) doubled vcpkg time and disk, and on
# GitHub's macOS runners the debug+release gRPC build ran out of disk
# ("ranlib: can't write to output file"). Debug builds of Cartwheel link
# these release libraries; the ABI is the same, since vcpkg's static libs do
# not use a debug iterator ABI.
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
