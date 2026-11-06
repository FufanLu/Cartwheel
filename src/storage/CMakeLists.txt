add_library(cartwheel_storage
  crc32c.cc
  mem_engine.cc
  posix_env.cc
  wal.cc
  lsm/arena.cc
  lsm/block.cc
  lsm/block_cache.cc
  lsm/bloom.cc
  lsm/db.cc
  lsm/iterator.cc
  lsm/memtable.cc
  lsm/table.cc
  lsm/version.cc
  lsm/write_batch.cc)
add_library(cartwheel::storage ALIAS cartwheel_storage)
cartwheel_target_defaults(cartwheel_storage)
target_link_libraries(cartwheel_storage PUBLIC cartwheel::common)
# ARMv8 CRC32C instructions (always present on Apple silicon / Graviton).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set_source_files_properties(crc32c.cc PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crc")
endif()
