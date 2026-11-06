#include "storage/wal.h"

#include <algorithm>

#include "common/coding.h"
#include "storage/crc32c.h"

namespace cartwheel::storage::wal {

Writer::Writer(std::unique_ptr<WritableFile> dest, uint64_t dest_length)
    : dest_(std::move(dest)), block_offset_(static_cast<size_t>(dest_length % kBlockSize)) {}

Status Writer::AddRecord(std::string_view record) {
  const char* ptr = record.data();
  size_t left = record.size();
  bool begin = true;
  // An empty record still emits one zero-length FULL fragment.
  do {
    const size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      // Not even a header fits: pad the block with zeros.
      if (leftover > 0) {
        static constexpr char kZeros[kHeaderSize] = {};
        CW_RETURN_IF_ERROR(dest_->Append(std::string_view(kZeros, leftover)));
      }
      block_offset_ = 0;
    }
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t n = std::min(left, avail);
    const bool end = (left == n);
    const RecordType type = begin && end ? kFullType
                            : begin      ? kFirstType
                            : end        ? kLastType
                                         : kMiddleType;
    CW_RETURN_IF_ERROR(EmitFragment(type, ptr, n));
    ptr += n;
    left -= n;
    begin = false;
  } while (left > 0);
  return dest_->Flush();
}

Status Writer::EmitFragment(RecordType type, const char* data, size_t n) {
  char header[kHeaderSize];
  const char type_byte = static_cast<char>(type);
  uint32_t crc = crc32c::Extend(0, &type_byte, 1);
  crc = crc32c::Extend(crc, data, n);
  EncodeFixed32(header, crc32c::Mask(crc));
  header[4] = static_cast<char>(n & 0xFF);
  header[5] = static_cast<char>((n >> 8) & 0xFF);
  header[6] = type_byte;
  CW_RETURN_IF_ERROR(dest_->Append(std::string_view(header, kHeaderSize)));
  CW_RETURN_IF_ERROR(dest_->Append(std::string_view(data, n)));
  block_offset_ += kHeaderSize + n;
  return Status::Ok();
}

Reader::Reader(std::unique_ptr<SequentialFile> src)
    : src_(std::move(src)), backing_(kBlockSize, '\0') {}

int Reader::ReadFragment(std::string_view* fragment) {
  for (;;) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // Anything left is block padding; refill with the next block.
        std::string_view got;
        Status s = src_->Read(kBlockSize, &got, backing_.data());
        if (!s.ok()) {
          status_ = std::move(s);
          eof_ = true;
          return kEof;
        }
        buffer_ = got;
        if (got.size() < kBlockSize) eof_ = true;
        continue;
      }
      // EOF with a partial header: a torn write at the tail.
      if (!buffer_.empty()) stats_.truncated_tail = true;
      buffer_ = {};
      return kEof;
    }

    const auto* h = reinterpret_cast<const unsigned char*>(buffer_.data());
    const size_t length = static_cast<size_t>(h[4]) | (static_cast<size_t>(h[5]) << 8);
    const auto type = static_cast<RecordType>(h[6]);
    if (kHeaderSize + length > buffer_.size()) {
      if (eof_) {
        // The fragment runs past the end of the file: torn tail, not corruption.
        stats_.truncated_tail = true;
        buffer_ = {};
        return kEof;
      }
      stats_.dropped_bytes += buffer_.size();
      buffer_ = {};
      return kBad;
    }
    if (type == kZeroType && length == 0) {
      // Zeroed region (e.g. preallocation): skip the rest of the block.
      buffer_ = {};
      continue;
    }
    // NOLINTBEGIN(bugprone-suspicious-stringview-data-usage): kHeaderSize + length <= size checked
    const uint32_t expected = crc32c::Unmask(DecodeFixed32(buffer_.data()));
    const uint32_t actual = crc32c::Value(buffer_.data() + 6, length + 1);
    // NOLINTEND(bugprone-suspicious-stringview-data-usage)
    if (expected != actual) {
      // The length field may itself be corrupt, so drop the whole rest of the block.
      stats_.dropped_bytes += buffer_.size();
      buffer_ = {};
      return kBad;
    }
    *fragment = buffer_.substr(kHeaderSize, length);
    buffer_.remove_prefix(kHeaderSize + length);
    return type;
  }
}

bool Reader::ReadRecord(std::string* record, bool paranoid) {
  record->clear();
  bool in_fragmented = false;
  for (;;) {
    std::string_view fragment;
    const int type = ReadFragment(&fragment);
    switch (type) {
      case kFullType:
        if (in_fragmented) stats_.dropped_bytes += record->size();
        record->assign(fragment);
        ++stats_.records;
        return true;
      case kFirstType:
        if (in_fragmented) stats_.dropped_bytes += record->size();
        record->assign(fragment);
        in_fragmented = true;
        break;
      case kMiddleType:
        if (!in_fragmented) {
          stats_.dropped_bytes += fragment.size();
        } else {
          record->append(fragment);
        }
        break;
      case kLastType:
        if (!in_fragmented) {
          stats_.dropped_bytes += fragment.size();
        } else {
          record->append(fragment);
          ++stats_.records;
          return true;
        }
        break;
      case kEof:
        if (in_fragmented) stats_.truncated_tail = true;
        record->clear();
        return false;
      case kBad:
        if (paranoid) {
          status_ = Status::Corruption("wal: checksum mismatch");
          return false;
        }
        if (in_fragmented) stats_.dropped_bytes += record->size();
        record->clear();
        in_fragmented = false;
        break;
      default:
        stats_.dropped_bytes += fragment.size();
        if (paranoid) {
          status_ = Status::Corruption("wal: unknown record type");
          return false;
        }
        in_fragmented = false;
        record->clear();
        break;
    }
  }
}

}  // namespace cartwheel::storage::wal
