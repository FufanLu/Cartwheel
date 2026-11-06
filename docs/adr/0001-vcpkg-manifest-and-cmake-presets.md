# ADR 0001: vcpkg manifest mode and CMake presets

- Status: accepted
- Date: 2026-11-06

## Context

Cartwheel depends on about a dozen C++ libraries (gRPC, Protobuf, Boost.Asio/Beast,
spdlog, prometheus-cpp, GoogleTest, …). It must build the same way on three
machines: a developer Mac (arm64), Linux CI runners (x86_64), and Docker images
(Linux arm64 or x86_64).

System package managers give us a different gRPC on each one. Homebrew ships the
latest release, while Ubuntu 24.04 apt still ships gRPC 1.51. That gap is large
enough that generated stubs and the callback API we use behave differently.

## Decision

- **vcpkg in manifest mode.** `vcpkg.json` lists every dependency. Its
  `builtin-baseline` pins the vcpkg registry to one commit, so each machine
  resolves exactly the same versions.
- **CMake presets** (`CMakePresets.json`) define the four supported builds:
  `dev`, `release` (RelWithDebInfo), `asan` (Address + UB sanitizers), and `tsan`.
  CI runs these presets as they are, so "works on my machine" and "works in CI"
  are the same command.
- Each dependency flavour has its own install tree. `dev` and `release` share
  `vcpkg_installed/default`, while `asan` and `tsan` use the sanitized overlay
  triplets in `triplets/`, each in its own tree. Sanitized code must link
  sanitized dependencies, because protobuf and abseil change their class
  layout under ASan and TSan. The trees must stay separate: in manifest mode,
  vcpkg treats an install root as wholly owned by a single install plan, and
  sharing one root across triplets made it delete the other triplet's
  packages.
- The top-level `CMakeLists.txt` finds the vcpkg toolchain from `$VCPKG_ROOT`
  first, then `~/vcpkg`, so the presets need no machine-specific paths.
- Docker builds start from `deploy/docker/builder.Dockerfile`. That image keeps
  the dependency layer keyed on `vcpkg.json`, so a code change never rebuilds gRPC.

## Consequences

- The first build on a machine compiles gRPC and its dependencies from source
  (tens of minutes). After that, vcpkg's binary cache makes it a no-op. CI keeps
  that cache in `actions/cache`, keyed on `vcpkg.json`.
- Upgrading a dependency is an explicit, reviewable change: bump the baseline
  or add a version override.
- Alternatives considered:
  - Conan is equally capable, but needs a Python toolchain and offers less
    CMake-native integration.
  - FetchContent for everything makes each clean build compile gRPC.
  - System packages lead to version skew, as described above.
