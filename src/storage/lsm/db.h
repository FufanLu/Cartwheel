#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "common/status.h"
#include "common/units.h"
#include "storage/engine.h"
#include "storage/env.h"
#include "storage/lsm/block_cache.h"
#include "storage/lsm/memtable.h"
#include "storage/lsm/version.h"
#include "storage/lsm/write_batch.h"
#include "storage/wal.h"

namespace cartwheel::storage::lsm {

struct Options {
  Env* env = Env::Default();
  size_t write_buffer_size = 4_MiB;  // memtable size before flush
  size_t block_cache_bytes = 32_MiB;
  size_t max_file_size = 2_MiB;  // compaction output split point
  TableOptions table;
  LevelTargets levels;
  int l0_slowdown_trigger = 8;  // writes sleep 1 ms each
  int l0_stop_trigger = 12;     // writes block until compaction catches up
  size_t max_batch_group_bytes = 1 << 20;
  // Called (from the background thread, without the DB mutex) after every
  // flush and compaction, with a one-line description. For ops logs and docs.
  std::function<void(const std::string&)> event_listener;
};

struct DbStats {
  uint64_t flushes = 0;
  uint64_t compactions = 0;
  uint64_t trivial_moves = 0;
  uint64_t bytes_written_user = 0;        // WAL payload bytes from callers
  uint64_t bytes_written_flush = 0;       // L0 tables
  uint64_t bytes_written_compaction = 0;  // L1+ tables
  uint64_t bytes_read_compaction = 0;
  uint64_t write_groups = 0;  // WAL records written (one per group commit)
  uint64_t writes = 0;        // caller Write() calls
  uint64_t stall_micros = 0;
  uint64_t slowdowns = 0;
  uint64_t block_cache_hits = 0;
  uint64_t block_cache_misses = 0;
  std::string levels;  // "L0:1 L1:3 ..."
  std::array<uint64_t, kNumLevels> level_bytes{};
  std::array<size_t, kNumLevels> level_files{};
  uint64_t memtable_bytes = 0;
  uint64_t memtable_entries = 0;
  uint64_t table_entries = 0;

  // Bytes written to disk per byte the user wrote (WAL + flush + compaction).
  [[nodiscard]] double WriteAmplification() const {
    if (bytes_written_user == 0) return 0;
    return static_cast<double>(bytes_written_user + bytes_written_flush +
                               bytes_written_compaction) /
           static_cast<double>(bytes_written_user);
  }
};

// Forward iterator over user keys at a snapshot: the newest visible version
// of each key, with tombstones and older versions skipped.
class DbIterator {
 public:
  DbIterator(std::unique_ptr<InternalIterator> internal, SequenceNumber snapshot,
             std::shared_ptr<const void> pin);
  [[nodiscard]] bool Valid() const { return valid_; }
  void SeekToFirst();
  void Seek(std::string_view user_key);
  void Next();
  [[nodiscard]] std::string_view key() const { return key_; }
  [[nodiscard]] std::string_view value() const { return internal_->value(); }
  [[nodiscard]] Status status() const { return internal_->status(); }

 private:
  void FindNextUserEntry(bool skipping_current);
  std::unique_ptr<InternalIterator> internal_;
  SequenceNumber snapshot_;
  std::shared_ptr<const void> pin_;  // keeps memtables and version alive
  bool valid_ = false;
  std::string key_;
};

// A leveled LSM-tree key-value store (LevelDB-style), the storage under
// every wheel-node.
//
//   Write: group commit -> one WAL record (+ optional fsync) -> memtable
//   Flush: full memtable -> L0 SSTable (background thread)
//   Compaction: L0 -> L1 -> ... merging overlapping tables, dropping
//               overwritten versions and obsolete tombstones
//   Read: memtable -> immutable memtable -> L0 (newest first) -> L1..L6,
//         a bloom filter per table skipping most misses
class DB {
 public:
  static Result<std::unique_ptr<DB>> Open(const Options& options, const std::string& dir);
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Write(const WriteOptions& opts, WriteBatch* batch);
  Status Put(const WriteOptions& opts, std::string_view key, std::string_view value);
  Status Delete(const WriteOptions& opts, std::string_view key);
  Result<std::string> Get(std::string_view key);  // NotFound if absent or deleted

  [[nodiscard]] std::unique_ptr<DbIterator> NewIterator();

  // Flushes the memtable to L0 and waits.
  Status Flush();
  // Compacts until no level is over its target, then waits.
  Status WaitForCompactions();

  [[nodiscard]] DbStats Stats() const;
  [[nodiscard]] const std::string& dir() const { return dir_; }

 private:
  struct Writer;

  DB(const Options& options, std::string dir);
  Status Recover();
  Status ReplayLog(uint64_t number, VersionEdit* edit);
  Status NewLogFile();

  Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  WriteBatch* BuildBatchGroup(Writer** last);

  void BackgroundLoop();
  void MaybeScheduleWork();
  Status FlushImmutable(std::unique_lock<std::mutex>& lock);
  Status RunCompaction(std::unique_lock<std::mutex>& lock, Compaction c);
  Status WriteLevel0Table(std::unique_lock<std::mutex>& lock, const std::shared_ptr<MemTable>& mem,
                          VersionEdit* edit);
  void DeleteObsoleteFiles();

  const Options options_;
  const std::string dir_;
  Env* const env_;
  std::unique_ptr<FileLock> lock_file_;
  BlockCache block_cache_;
  TableCache table_cache_;

  mutable std::mutex mu_;
  std::condition_variable bg_cv_;    // background work available / shutdown
  std::condition_variable done_cv_;  // background work finished (stalls, Flush waits)
  VersionSet versions_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;
  std::unique_ptr<wal::Writer> log_;
  uint64_t log_number_ = 0;
  std::deque<Writer*> writers_;
  WriteBatch group_batch_;
  std::set<uint64_t> pending_outputs_;
  Status bg_error_;
  bool shutting_down_ = false;
  bool bg_busy_ = false;
  std::thread bg_thread_;

  DbStats stats_;  // guarded by mu_
};

// storage::Engine adapter so wheel-node can run on the LSM tree.
Result<std::unique_ptr<Engine>> NewLsmEngine(const Options& options, const std::string& dir);

}  // namespace cartwheel::storage::lsm
