#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "common/status.h"

namespace cartwheel::storage {

struct WriteOptions {
  // fsync the WAL before acknowledging. Off by default: replication across
  // N nodes, not a single disk flush, is what makes an acked write durable.
  bool sync = false;
};

// Forward iterator over a consistent view of the engine.
class Iterator {
 public:
  virtual ~Iterator() = default;
  [[nodiscard]] virtual bool Valid() const = 0;
  virtual void SeekToFirst() = 0;
  virtual void Seek(std::string_view target) = 0;  // first key >= target
  virtual void Next() = 0;
  [[nodiscard]] virtual std::string_view key() const = 0;
  [[nodiscard]] virtual std::string_view value() const = 0;
  [[nodiscard]] virtual Status status() const = 0;
};

struct EngineStats {
  uint64_t keys = 0;
  uint64_t bytes = 0;  // key + value bytes currently stored
};

// Single-node ordered key-value engine. Keys and values are opaque bytes;
// CartwheelKV stores an encoded SiblingSet per key.
//
// Implementations must be safe for concurrent use. Each call is atomic, but
// read-modify-write sequences are not: the KV node serializes those per key.
class Engine {
 public:
  virtual ~Engine() = default;

  virtual Status Put(std::string_view key, std::string_view value, const WriteOptions& opts) = 0;
  virtual Status Delete(std::string_view key, const WriteOptions& opts) = 0;
  // NotFound if absent.
  [[nodiscard]] virtual Result<std::string> Get(std::string_view key) const = 0;
  [[nodiscard]] virtual std::unique_ptr<Iterator> NewIterator() const = 0;
  [[nodiscard]] virtual EngineStats Stats() const = 0;
  [[nodiscard]] virtual std::string_view Name() const = 0;

  Status Put(std::string_view key, std::string_view value) { return Put(key, value, {}); }
  Status Delete(std::string_view key) { return Delete(key, {}); }
};

// In-memory engine (std::map + shared_mutex). Used by M1 and as the reference
// model the LSM engine is differential-tested against.
[[nodiscard]] std::unique_ptr<Engine> NewMemEngine();

}  // namespace cartwheel::storage
