#include "storage/lsm/version.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include <fmt/format.h>

#include "common/coding.h"

namespace cartwheel::storage::lsm {

// ---- file names ----------------------------------------------------------------

std::string TableFileName(const std::string& dir, uint64_t n) {
  return fmt::format("{}/{:06d}.sst", dir, n);
}
std::string LogFileName(const std::string& dir, uint64_t n) {
  return fmt::format("{}/{:06d}.log", dir, n);
}
std::string ManifestFileName(const std::string& dir, uint64_t n) {
  return fmt::format("{}/MANIFEST-{:06d}", dir, n);
}
std::string CurrentFileName(const std::string& dir) {
  return dir + "/CURRENT";
}

FileType ParseFileName(const std::string& name, uint64_t* number) {
  if (name == "CURRENT") return FileType::kCurrent;
  if (name == "LOCK") return FileType::kLock;
  if (name.ends_with(".tmp")) return FileType::kTemp;
  auto parse = [&](std::string_view digits) {
    if (digits.empty()) return false;
    uint64_t v = 0;
    for (const char c : digits) {
      if (c < '0' || c > '9') return false;
      v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    *number = v;
    return true;
  };
  if (name.starts_with("MANIFEST-") && parse(std::string_view(name).substr(9))) {
    return FileType::kManifest;
  }
  if (name.ends_with(".sst") && parse(std::string_view(name).substr(0, name.size() - 4))) {
    return FileType::kTable;
  }
  if (name.ends_with(".log") && parse(std::string_view(name).substr(0, name.size() - 4))) {
    return FileType::kLog;
  }
  return FileType::kUnknown;
}

// ---- version edit --------------------------------------------------------------

namespace {
enum Tag : uint8_t {
  kLogNumber = 1,
  kNextFileNumber = 2,
  kLastSequence = 3,
  kDeletedFile = 4,
  kNewFile = 5,
};
}  // namespace

void VersionEdit::EncodeTo(std::string* dst) const {
  if (log_number) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, *log_number);
  }
  if (next_file_number) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, *next_file_number);
  }
  if (last_sequence) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, *last_sequence);
  }
  for (const auto& [level, number] : deleted_files) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, number);
  }
  for (const auto& [level, f] : new_files) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, static_cast<uint32_t>(level));
    PutVarint64(dst, f.number);
    PutVarint64(dst, f.file_size);
    PutVarint64(dst, f.num_entries);
    PutLengthPrefixed(dst, f.smallest);
    PutLengthPrefixed(dst, f.largest);
  }
}

Status VersionEdit::DecodeFrom(std::string_view src) {
  *this = VersionEdit{};
  Decoder d(src);
  while (!d.empty()) {
    uint32_t tag = 0;
    if (!d.GetVarint32(&tag)) return Status::Corruption("manifest: bad tag");
    uint64_t v = 0;
    switch (tag) {
      case kLogNumber:
        if (!d.GetVarint64(&v)) return Status::Corruption("manifest: log number");
        log_number = v;
        break;
      case kNextFileNumber:
        if (!d.GetVarint64(&v)) return Status::Corruption("manifest: next file");
        next_file_number = v;
        break;
      case kLastSequence:
        if (!d.GetVarint64(&v)) return Status::Corruption("manifest: last sequence");
        last_sequence = v;
        break;
      case kDeletedFile: {
        uint32_t level = 0;
        if (!d.GetVarint32(&level) || level >= kNumLevels || !d.GetVarint64(&v)) {
          return Status::Corruption("manifest: deleted file");
        }
        deleted_files.emplace(static_cast<int>(level), v);
        break;
      }
      case kNewFile: {
        uint32_t level = 0;
        FileMetaData f;
        std::string_view smallest;
        std::string_view largest;
        if (!d.GetVarint32(&level) || level >= kNumLevels || !d.GetVarint64(&f.number) ||
            !d.GetVarint64(&f.file_size) || !d.GetVarint64(&f.num_entries) ||
            !d.GetLengthPrefixed(&smallest) || !d.GetLengthPrefixed(&largest)) {
          return Status::Corruption("manifest: new file");
        }
        f.smallest = std::string(smallest);
        f.largest = std::string(largest);
        new_files.emplace_back(static_cast<int>(level), std::move(f));
        break;
      }
      default: return Status::Corruption(fmt::format("manifest: unknown tag {}", tag));
    }
  }
  return Status::Ok();
}

std::string VersionEdit::DebugString() const {
  std::string out = "{";
  if (log_number) out += fmt::format(" log={}", *log_number);
  if (next_file_number) out += fmt::format(" next_file={}", *next_file_number);
  if (last_sequence) out += fmt::format(" last_seq={}", *last_sequence);
  for (const auto& [l, n] : deleted_files) out += fmt::format(" -L{}#{}", l, n);
  for (const auto& [l, f] : new_files) {
    out += fmt::format(" +L{}#{}({}B,{} keys)", l, f.number, f.file_size, f.num_entries);
  }
  return out + " }";
}

// ---- table cache ----------------------------------------------------------------

TableCache::TableCache(std::string dir, Env* env, BlockCache* blocks)
    : dir_(std::move(dir)), env_(env), blocks_(blocks) {}

Result<std::shared_ptr<const Table>> TableCache::Find(uint64_t number) {
  {
    std::lock_guard lock(mu_);
    if (auto it = open_.find(number); it != open_.end()) return it->second;
  }
  CW_ASSIGN_OR_RETURN(auto file, env_->NewRandomAccessFile(TableFileName(dir_, number)));
  CW_ASSIGN_OR_RETURN(std::shared_ptr<const Table> table,
                      Table::Open(std::move(file), number, blocks_));
  std::lock_guard lock(mu_);
  auto [it, inserted] = open_.emplace(number, table);
  return it->second;
}

void TableCache::Evict(uint64_t number) {
  {
    std::lock_guard lock(mu_);
    open_.erase(number);
  }
  if (blocks_ != nullptr) blocks_->EraseFile(number);
}

// ---- version ---------------------------------------------------------------------

Result<MemTable::GetResult> Version::Get(const LookupKey& key, std::string* value,
                                         TableCache* tables) const {
  const std::string_view user_key = key.user_key();
  auto in_range = [&](const FileMetaData& f) {
    return user_key >= ExtractUserKey(f.smallest) && user_key <= ExtractUserKey(f.largest);
  };
  // L0 files overlap: check every candidate, newest (highest number) first.
  for (const FilePtr& f : files[0]) {
    if (!in_range(*f)) continue;
    CW_ASSIGN_OR_RETURN(auto table, tables->Find(f->number));
    CW_ASSIGN_OR_RETURN(auto r, table->Get(key, value));
    if (r != MemTable::GetResult::kNotFound) return r;
  }
  // Deeper levels: at most one file per level can hold the key.
  for (int level = 1; level < kNumLevels; ++level) {
    const auto& level_files = files[level];
    auto it = std::lower_bound(
        level_files.begin(), level_files.end(), key.internal_key(),
        [](const FilePtr& f, std::string_view k) { return CompareInternalKey(f->largest, k) < 0; });
    if (it == level_files.end() || user_key < ExtractUserKey((*it)->smallest)) continue;
    CW_ASSIGN_OR_RETURN(auto table, tables->Find((*it)->number));
    CW_ASSIGN_OR_RETURN(auto r, table->Get(key, value));
    if (r != MemTable::GetResult::kNotFound) return r;
  }
  return MemTable::GetResult::kNotFound;
}

void Version::AddIterators(TableCache* tables,
                           std::vector<std::unique_ptr<InternalIterator>>* out) const {
  for (const auto& level_files : files) {
    for (const FilePtr& f : level_files) {
      auto table = tables->Find(f->number);
      if (!table.ok()) {
        out->push_back(NewErrorIterator(table.status()));
      } else {
        out->push_back(Table::NewIterator(std::move(table).value()));
      }
    }
  }
}

uint64_t Version::LevelBytes(int level) const {
  uint64_t total = 0;
  for (const FilePtr& f : files[level]) total += f->file_size;
  return total;
}

bool Version::IsBaseLevelForKey(int level, std::string_view user_key) const {
  for (int l = level + 1; l < kNumLevels; ++l) {
    for (const FilePtr& f : files[l]) {
      if (user_key >= ExtractUserKey(f->smallest) && user_key <= ExtractUserKey(f->largest)) {
        return false;
      }
    }
  }
  return true;
}

std::vector<FilePtr> Version::Overlapping(int level, std::string_view begin_user,
                                          std::string_view end_user) const {
  std::vector<FilePtr> out;
  for (const FilePtr& f : files[level]) {
    if (ExtractUserKey(f->largest) < begin_user || ExtractUserKey(f->smallest) > end_user) continue;
    out.push_back(f);
  }
  return out;
}

std::string Version::Summary() const {
  std::string out;
  for (int l = 0; l < kNumLevels; ++l) {
    if (l > 0) out += ' ';
    out += fmt::format("L{}:{}", l, files[l].size());
  }
  return out;
}

// ---- version set ------------------------------------------------------------------

VersionSet::VersionSet(std::string dir, Env* env, LevelTargets targets)
    : dir_(std::move(dir)),
      env_(env),
      targets_(targets),
      current_(std::make_shared<const Version>()) {}

std::shared_ptr<const Version> VersionSet::Apply(const Version& base,
                                                 const VersionEdit& edit) const {
  auto v = std::make_shared<Version>();
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FilePtr& f : base.files[l]) {
      if (!edit.deleted_files.contains({l, f->number})) v->files[l].push_back(f);
    }
  }
  for (const auto& [level, meta] : edit.new_files) {
    if (!edit.deleted_files.contains({level, meta.number})) {
      v->files[level].push_back(std::make_shared<const FileMetaData>(meta));
    }
  }
  // L0 newest first; other levels by smallest key.
  std::sort(v->files[0].begin(), v->files[0].end(),
            [](const FilePtr& a, const FilePtr& b) { return a->number > b->number; });
  for (int l = 1; l < kNumLevels; ++l) {
    std::sort(v->files[l].begin(), v->files[l].end(), [](const FilePtr& a, const FilePtr& b) {
      return CompareInternalKey(a->smallest, b->smallest) < 0;
    });
  }
  return v;
}

Status VersionSet::WriteSnapshot(wal::Writer* log) const {
  VersionEdit snap;
  snap.log_number = log_number_;
  snap.next_file_number = next_file_number_;
  snap.last_sequence = last_sequence_;
  for (int l = 0; l < kNumLevels; ++l) {
    for (const FilePtr& f : current_->files[l]) snap.new_files.emplace_back(l, *f);
  }
  std::string rec;
  snap.EncodeTo(&rec);
  return log->AddRecord(rec);
}

Status VersionSet::Recover() {
  const std::string current_path = CurrentFileName(dir_);
  if (env_->FileExists(current_path)) {
    CW_ASSIGN_OR_RETURN(std::string current, ReadFileToString(env_, current_path));
    if (current.empty() || current.back() != '\n') {
      return Status::Corruption("CURRENT file does not end with a newline");
    }
    current.pop_back();
    CW_ASSIGN_OR_RETURN(auto file, env_->NewSequentialFile(dir_ + "/" + current));
    wal::Reader reader(std::move(file));
    std::string record;
    std::shared_ptr<const Version> v = std::make_shared<const Version>();
    while (reader.ReadRecord(&record, /*paranoid=*/true)) {
      VersionEdit edit;
      CW_RETURN_IF_ERROR(edit.DecodeFrom(record).WithContext(current));
      v = Apply(*v, edit);
      if (edit.log_number) log_number_ = *edit.log_number;
      if (edit.next_file_number) next_file_number_ = *edit.next_file_number;
      if (edit.last_sequence) last_sequence_ = *edit.last_sequence;
      for (const auto& [l, f] : edit.new_files) MarkFileNumberUsed(f.number);
    }
    CW_RETURN_IF_ERROR(reader.status().WithContext(current));
    current_ = v;
    uint64_t old_manifest = 0;
    if (ParseFileName(current, &old_manifest) == FileType::kManifest) {
      MarkFileNumberUsed(old_manifest);
    }
  }
  // Always start a fresh MANIFEST holding a full snapshot, so the file never
  // grows without bound across restarts.
  manifest_number_ = NewFileNumber();
  CW_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(ManifestFileName(dir_, manifest_number_)));
  manifest_ = std::make_unique<wal::Writer>(std::move(file));
  CW_RETURN_IF_ERROR(WriteSnapshot(manifest_.get()));
  CW_RETURN_IF_ERROR(manifest_->Sync());
  CW_RETURN_IF_ERROR(
      WriteFileAtomically(env_, current_path, fmt::format("MANIFEST-{:06d}\n", manifest_number_)));
  live_.push_back(current_);
  return Status::Ok();
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (!edit->next_file_number) edit->next_file_number = next_file_number_;
  if (!edit->last_sequence) edit->last_sequence = last_sequence_;
  if (!edit->log_number) edit->log_number = log_number_;
  std::string rec;
  edit->EncodeTo(&rec);
  CW_RETURN_IF_ERROR(manifest_->AddRecord(rec));
  // The edit must be durable before the new version is visible: after a
  // crash, recovery rebuilds exactly what readers may have observed.
  CW_RETURN_IF_ERROR(manifest_->Sync());
  current_ = Apply(*current_, *edit);
  log_number_ = *edit->log_number;
  live_.push_back(current_);
  return Status::Ok();
}

uint64_t VersionSet::MaxBytesForLevel(int level) const {
  uint64_t bytes = targets_.l1_target_bytes;
  for (int l = 1; l < level; ++l) bytes *= static_cast<uint64_t>(targets_.level_multiplier);
  return bytes;
}

double VersionSet::CompactionScore(int* level) const {
  double best = static_cast<double>(current_->files[0].size()) /
                static_cast<double>(targets_.l0_compaction_trigger);
  *level = 0;
  for (int l = 1; l < kNumLevels - 1; ++l) {
    const double score =
        static_cast<double>(current_->LevelBytes(l)) / static_cast<double>(MaxBytesForLevel(l));
    if (score > best) {
      best = score;
      *level = l;
    }
  }
  return best;
}

std::optional<Compaction> VersionSet::PickCompaction() {
  int level = 0;
  if (CompactionScore(&level) < 1.0) return std::nullopt;
  Compaction c;
  c.level = level;
  c.input_version = current_;
  const Version& v = *current_;
  if (level == 0) {
    c.inputs[0] = v.files[0];  // L0 files overlap: take them all
  } else {
    // Round-robin through the level so every key range gets compacted.
    const auto& files = v.files[level];
    auto it = std::find_if(files.begin(), files.end(), [&](const FilePtr& f) {
      return compact_pointer_[level].empty() ||
             CompareInternalKey(f->largest, compact_pointer_[level]) > 0;
    });
    if (it == files.end()) it = files.begin();
    c.inputs[0].push_back(*it);
    compact_pointer_[level] = (*it)->largest;
  }
  std::string_view lo = ExtractUserKey(c.inputs[0].front()->smallest);
  std::string_view hi = ExtractUserKey(c.inputs[0].front()->largest);
  for (const FilePtr& f : c.inputs[0]) {
    lo = std::min(lo, ExtractUserKey(f->smallest));
    hi = std::max(hi, ExtractUserKey(f->largest));
  }
  c.inputs[1] = v.Overlapping(level + 1, lo, hi);
  return c;
}

std::set<uint64_t> VersionSet::LiveFiles() {
  std::set<uint64_t> live;
  for (auto it = live_.begin(); it != live_.end();) {
    if (auto v = it->lock()) {
      for (const auto& level_files : v->files) {
        for (const FilePtr& f : level_files) live.insert(f->number);
      }
      ++it;
    } else {
      it = live_.erase(it);
    }
  }
  return live;
}

}  // namespace cartwheel::storage::lsm
