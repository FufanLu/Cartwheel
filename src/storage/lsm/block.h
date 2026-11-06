#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/coding.h"
#include "common/status.h"
#include "storage/env.h"
#include "storage/lsm/iterator.h"

namespace cartwheel::storage::lsm {

// ---- block handles and the table footer -------------------------------------

struct BlockHandle {
  uint64_t offset = 0;
  uint64_t size = 0;  // excluding the 5-byte trailer
  void EncodeTo(std::string* dst) const;
  [[nodiscard]] bool DecodeFrom(Decoder* in);
};

// Every block on disk is followed by: u8 compression (0 = none) | fixed32 masked crc32c(block ||
// type).
inline constexpr size_t kBlockTrailerSize = 5;

// Fixed-size footer at the end of every SSTable:
//   index handle | filter handle | zero padding to 40 bytes | fixed64 magic
struct Footer {
  static constexpr size_t kEncodedLength = 48;
  // Stored little-endian, so the bytes read "CARTWHL" then 0x01 (format version 1).
  static constexpr uint64_t kMagic = 0x014C485754524143ULL;
  BlockHandle index;
  BlockHandle filter;
  void EncodeTo(std::string* dst) const;
  [[nodiscard]] Status DecodeFrom(std::string_view in);
};

// Reads a block and verifies its checksum.
Result<std::string> ReadBlock(const RandomAccessFile& file, const BlockHandle& handle);

// ---- block builder ------------------------------------------------------------

// Keys inside a block are prefix-compressed against their predecessor:
//   varint shared | varint unshared | varint value_len | key[shared:] | value
// Every `restart_interval` entries the key is stored whole (a restart point).
// The trailing array of fixed32 restart offsets allows binary search, so a
// seek costs O(log restarts) comparisons plus at most `interval` linear steps.
class BlockBuilder {
 public:
  explicit BlockBuilder(int restart_interval = 16);
  void Add(std::string_view key, std::string_view value);  // keys strictly increasing
  std::string_view Finish();
  void Reset();
  [[nodiscard]] size_t CurrentSizeEstimate() const;
  [[nodiscard]] bool empty() const { return buffer_.empty(); }

 private:
  const int restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_ = 0;
  bool finished_ = false;
  std::string last_key_;
};

// ---- block reader --------------------------------------------------------------

class Block {
 public:
  explicit Block(std::string contents);
  [[nodiscard]] size_t size() const { return data_.size(); }
  [[nodiscard]] bool ok() const { return num_restarts_ > 0 || data_.size() == sizeof(uint32_t); }
  [[nodiscard]] uint32_t num_restarts() const { return num_restarts_; }

  // Keys compared as internal keys. The iterator holds `self` alive.
  static std::unique_ptr<InternalIterator> NewIterator(std::shared_ptr<const Block> self);

 private:
  friend class BlockIterator;
  std::string data_;
  uint32_t restart_offset_ = 0;
  uint32_t num_restarts_ = 0;
};

}  // namespace cartwheel::storage::lsm
