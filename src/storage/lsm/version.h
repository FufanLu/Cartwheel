#pragma once

#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/status.h"
#include "common/units.h"
#include "storage/env.h"
#include "storage/lsm/memtable.h"
#include "storage/lsm/table.h"
#include "storage/wal.h"

namespace cartwheel::storage::lsm {

inline constexpr int kNumLevels = 7;

struct FileMetaData {
  uint64_t number = 0;
  uint64_t file_size = 0;
  uint64_t num_entries = 0;
  std::string smallest;  // internal keys
  std::string largest;
};
using FilePtr = std::shared_ptr<const FileMetaData>;

// A delta to the set of live files, recorded as one MANIFEST record.
// Recovery replays the MANIFEST: state(n) = state(n-1) + edit(n).
struct VersionEdit {
  std::optional<uint64_t> log_number;
  std::optional<uint64_t> next_file_number;
  std::optional<uint64_t> last_sequence;
  std::vector<std::pair<int, FileMetaData>> new_files;
  std::set<std::pair<int, uint64_t>> deleted_files;

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(std::string_view src);
  [[nodiscard]] std::string DebugString() const;
};

// Opens SSTables by file number and keeps them open (fds + index + filter).
class TableCache {
 public:
  TableCache(std::string dir, Env* env, BlockCache* blocks);
  Result<std::shared_ptr<const Table>> Find(uint64_t number);
  void Evict(uint64_t number);

 private:
  std::string dir_;
  Env* env_;
  BlockCache* blocks_;
  std::mutex mu_;
  std::unordered_map<uint64_t, std::shared_ptr<const Table>> open_;
};

// An immutable snapshot of which SSTables make up the database.
//   L0: flushed memtables, may overlap, searched newest-first.
//   L1..L6: each level is sorted and non-overlapping, ~10x the size of the one above.
// Readers hold a shared_ptr<const Version>, so a compaction that installs a
// new version never pulls files out from under them.
class Version {
 public:
  std::array<std::vector<FilePtr>, kNumLevels> files;

  Result<MemTable::GetResult> Get(const LookupKey& key, std::string* value,
                                  TableCache* tables) const;
  void AddIterators(TableCache* tables, std::vector<std::unique_ptr<InternalIterator>>* out) const;

  [[nodiscard]] uint64_t LevelBytes(int level) const;
  // True if no file at `level+1` or deeper may contain user_key (tombstones
  // can be dropped when compacting into the bottom of the key's range).
  [[nodiscard]] bool IsBaseLevelForKey(int level, std::string_view user_key) const;
  // Files at `level` whose user-key range overlaps [begin, end].
  [[nodiscard]] std::vector<FilePtr> Overlapping(int level, std::string_view begin_user,
                                                 std::string_view end_user) const;
  [[nodiscard]] std::string Summary() const;  // "L0:2 L1:5 L2:0 ..."
};

struct LevelTargets {
  int l0_compaction_trigger = 4;
  uint64_t l1_target_bytes = 10_MiB;
  int level_multiplier = 10;
};

// Work for one compaction: inputs[0] at `level`, inputs[1] at `level + 1`.
struct Compaction {
  int level = 0;
  std::array<std::vector<FilePtr>, 2> inputs;
  std::shared_ptr<const Version> input_version;
  [[nodiscard]] bool IsTrivialMove() const {
    // One file with nothing below it can just be re-linked one level down.
    return level > 0 && inputs[0].size() == 1 && inputs[1].empty();
  }
};

class VersionSet {
 public:
  VersionSet(std::string dir, Env* env, LevelTargets targets);

  // Loads CURRENT -> MANIFEST, or initialises an empty DB. Always starts a new MANIFEST.
  Status Recover();
  // REQUIRES: external mutex held (DB::mu_). Writes the edit to the MANIFEST
  // (fsync) before installing the new current version.
  Status LogAndApply(VersionEdit* edit);

  [[nodiscard]] std::shared_ptr<const Version> current() const { return current_; }
  uint64_t NewFileNumber() { return next_file_number_++; }
  void MarkFileNumberUsed(uint64_t n) {
    if (n >= next_file_number_) next_file_number_ = n + 1;
  }
  [[nodiscard]] uint64_t last_sequence() const { return last_sequence_; }
  void SetLastSequence(uint64_t s) { last_sequence_ = s; }
  [[nodiscard]] uint64_t log_number() const { return log_number_; }
  [[nodiscard]] uint64_t manifest_number() const { return manifest_number_; }

  // Highest-priority compaction, if any level is over its target.
  [[nodiscard]] std::optional<Compaction> PickCompaction();
  [[nodiscard]] double CompactionScore(int* level) const;

  // File numbers referenced by any version still alive (current or held by readers).
  [[nodiscard]] std::set<uint64_t> LiveFiles();

 private:
  std::shared_ptr<const Version> Apply(const Version& base, const VersionEdit& edit) const;
  Status WriteSnapshot(wal::Writer* log) const;
  [[nodiscard]] uint64_t MaxBytesForLevel(int level) const;

  const std::string dir_;
  Env* const env_;
  const LevelTargets targets_;
  uint64_t next_file_number_ = 2;
  uint64_t manifest_number_ = 0;
  uint64_t last_sequence_ = 0;
  uint64_t log_number_ = 0;
  std::shared_ptr<const Version> current_;
  std::list<std::weak_ptr<const Version>> live_;
  std::unique_ptr<wal::Writer> manifest_;
  std::array<std::string, kNumLevels> compact_pointer_;  // round-robin cursor per level
};

// File naming inside a DB directory.
std::string TableFileName(const std::string& dir, uint64_t number);
std::string LogFileName(const std::string& dir, uint64_t number);
std::string ManifestFileName(const std::string& dir, uint64_t number);
std::string CurrentFileName(const std::string& dir);
enum class FileType : uint8_t { kLog, kTable, kManifest, kCurrent, kLock, kTemp, kUnknown };
FileType ParseFileName(const std::string& name, uint64_t* number);

}  // namespace cartwheel::storage::lsm
