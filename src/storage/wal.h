#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "common/status.h"
#include "common/units.h"
#include "storage/env.h"

namespace cartwheel::storage::wal {

// Write-ahead log in the LevelDB record format. The file is a sequence of
// 32 KiB blocks. A record that does not fit in the rest of a block is split
// into FIRST / MIDDLE... / LAST fragments, and each fragment carries a header:
//
//   +---------+-----------+--------+-------------------+
//   | crc32c  | length    | type   | payload           |
//   | 4 bytes | 2 bytes LE| 1 byte | `length` bytes    |
//   +---------+-----------+--------+-------------------+
//   crc = Mask(crc32c(type || payload))
//
// Blocks let the reader resynchronise after corruption. A bad fragment costs
// at most the rest of its 32 KiB block, never the rest of the log. A torn
// write at the tail (power lost mid-append) is recognised and ignored,
// because a write that never completed was never acknowledged.

inline constexpr size_t kBlockSize = 32_KiB;
inline constexpr size_t kHeaderSize = 4 + 2 + 1;

enum RecordType : uint8_t {
  kZeroType = 0,  // preallocated / zeroed tail
  kFullType = 1,
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

class Writer {
 public:
  // `dest` must be empty, or positioned at its current end (dest_length).
  explicit Writer(std::unique_ptr<WritableFile> dest, uint64_t dest_length = 0);

  Status AddRecord(std::string_view record);
  Status Sync() { return dest_->Sync(); }
  Status Close() { return dest_->Close(); }
  [[nodiscard]] uint64_t size() const { return dest_->Size(); }

 private:
  Status EmitFragment(RecordType type, const char* data, size_t n);

  std::unique_ptr<WritableFile> dest_;
  size_t block_offset_;
};

struct ReadStats {
  uint64_t records = 0;
  uint64_t dropped_bytes = 0;   // skipped because of corruption
  bool truncated_tail = false;  // log ended inside a record (torn write)
};

class Reader {
 public:
  explicit Reader(std::unique_ptr<SequentialFile> src);

  // Next complete record. Returns false at end of log. Corrupt fragments are
  // skipped and counted in stats(). With paranoid = true the first corruption
  // stops reading (used for the MANIFEST, where skipping is unsafe).
  bool ReadRecord(std::string* record, bool paranoid = false);

  [[nodiscard]] const ReadStats& stats() const noexcept { return stats_; }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

 private:
  static constexpr int kEof = 5;  // pseudo record types returned by ReadFragment
  static constexpr int kBad = 6;
  int ReadFragment(std::string_view* fragment);

  std::unique_ptr<SequentialFile> src_;
  std::string backing_;
  std::string_view buffer_;
  bool eof_ = false;
  ReadStats stats_;
  Status status_;
};

}  // namespace cartwheel::storage::wal
