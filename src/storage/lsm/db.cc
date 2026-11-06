#include "storage/lsm/db.h"

#include <algorithm>
#include <chrono>
#include <tuple>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "common/units.h"

namespace cartwheel::storage::lsm {

namespace {

int64_t NowMicros() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Writes every entry of `it` into a new table file; fills meta (size, range, entries).
Status BuildTable(Env* env, const std::string& path, const TableOptions& topts,
                  InternalIterator* it, FileMetaData* meta) {
  CW_ASSIGN_OR_RETURN(auto file, env->NewWritableFile(path));
  TableBuilder builder(topts, file.get());
  it->SeekToFirst();
  for (; it->Valid(); it->Next()) {
    if (meta->num_entries == 0) meta->smallest.assign(it->key());
    meta->largest.assign(it->key());
    builder.Add(it->key(), it->value());
    ++meta->num_entries;
  }
  CW_RETURN_IF_ERROR(it->status());
  CW_RETURN_IF_ERROR(builder.Finish());
  CW_RETURN_IF_ERROR(file->Sync());
  CW_RETURN_IF_ERROR(file->Close());
  meta->file_size = builder.FileSize();
  if (meta->num_entries == 0) return env->RemoveFile(path);
  return Status::Ok();
}

}  // namespace

// ---- DbIterator -------------------------------------------------------------------

DbIterator::DbIterator(std::unique_ptr<InternalIterator> internal, SequenceNumber snapshot,
                       std::shared_ptr<const void> pin)
    : internal_(std::move(internal)), snapshot_(snapshot), pin_(std::move(pin)) {}

void DbIterator::SeekToFirst() {
  internal_->SeekToFirst();
  FindNextUserEntry(false);
}

void DbIterator::Seek(std::string_view user_key) {
  internal_->Seek(MakeInternalKey(user_key, snapshot_, kValueTypeForSeek));
  FindNextUserEntry(false);
}

void DbIterator::Next() {
  internal_->Next();
  FindNextUserEntry(true);
}

void DbIterator::FindNextUserEntry(bool skipping_current) {
  // Internal keys arrive as (user key asc, seq desc). For each user key the
  // first entry at or below the snapshot is the visible one. Everything
  // after it for that user key is an older version, so it is skipped.
  std::string skip = skipping_current ? key_ : std::string();
  bool has_skip = skipping_current;
  for (; internal_->Valid(); internal_->Next()) {
    ParsedInternalKey p;
    if (!ParseInternalKey(internal_->key(), &p)) continue;
    if (p.sequence > snapshot_) continue;  // written after this iterator was created
    if (has_skip && p.user_key == skip) continue;
    if (p.type == ValueType::kDeletion) {
      skip.assign(p.user_key);  // hide every older version of a deleted key
      has_skip = true;
      continue;
    }
    key_.assign(p.user_key);
    valid_ = true;
    return;
  }
  valid_ = false;
}

// ---- DB -------------------------------------------------------------------------------

struct DB::Writer {
  WriteBatch* batch = nullptr;  // nullptr: force a memtable switch (Flush)
  bool sync = false;
  bool done = false;
  Status status;
  std::condition_variable cv;
};

DB::DB(const Options& options, std::string dir)
    : options_(options),
      dir_(std::move(dir)),
      env_(options.env),
      block_cache_(options.block_cache_bytes),
      table_cache_(dir_, env_, &block_cache_),
      versions_(dir_, env_, options.levels),
      mem_(std::make_shared<MemTable>()) {}

Result<std::unique_ptr<DB>> DB::Open(const Options& options, const std::string& dir) {
  CW_RETURN_IF_ERROR(options.env->CreateDirIfMissing(dir));
  std::unique_ptr<DB> db(new DB(options, dir));
  CW_ASSIGN_OR_RETURN(db->lock_file_, options.env->LockFile(dir + "/LOCK"));
  CW_RETURN_IF_ERROR(db->Recover());
  db->bg_thread_ = std::thread([raw = db.get()] { raw->BackgroundLoop(); });
  return db;
}

DB::~DB() {
  {
    std::lock_guard lock(mu_);
    shutting_down_ = true;
  }
  bg_cv_.notify_all();
  if (bg_thread_.joinable()) bg_thread_.join();
  if (log_) static_cast<void>(log_->Close());
}

Status DB::NewLogFile() {
  const uint64_t number = versions_.NewFileNumber();
  CW_ASSIGN_OR_RETURN(auto file, env_->NewWritableFile(LogFileName(dir_, number)));
  if (log_) CW_RETURN_IF_ERROR(log_->Close());
  log_ = std::make_unique<wal::Writer>(std::move(file));
  log_number_ = number;
  return Status::Ok();
}

Status DB::Recover() {
  std::unique_lock lock(mu_);
  CW_RETURN_IF_ERROR(versions_.Recover());

  // Replay every WAL not yet covered by a flushed table, oldest first.
  CW_ASSIGN_OR_RETURN(auto names, env_->ListDir(dir_));
  std::vector<uint64_t> logs;
  for (const auto& name : names) {
    uint64_t number = 0;
    if (ParseFileName(name, &number) == FileType::kLog && number >= versions_.log_number()) {
      logs.push_back(number);
    }
  }
  std::sort(logs.begin(), logs.end());
  VersionEdit edit;
  for (const uint64_t number : logs) {
    versions_.MarkFileNumberUsed(number);
    CW_RETURN_IF_ERROR(ReplayLog(number, &edit));
  }
  if (mem_->entries() > 0) {
    CW_RETURN_IF_ERROR(WriteLevel0Table(lock, mem_, &edit));
    mem_ = std::make_shared<MemTable>();
  }
  CW_RETURN_IF_ERROR(NewLogFile());
  edit.log_number = log_number_;
  edit.last_sequence = versions_.last_sequence();
  CW_RETURN_IF_ERROR(versions_.LogAndApply(&edit));
  DeleteObsoleteFiles();
  if (!logs.empty()) {
    SPDLOG_INFO("lsm: recovered {} WAL file(s), last_seq={}, {}", logs.size(),
                versions_.last_sequence(), versions_.current()->Summary());
  }
  return Status::Ok();
}

Status DB::ReplayLog(uint64_t number, VersionEdit* edit) {
  CW_ASSIGN_OR_RETURN(auto file, env_->NewSequentialFile(LogFileName(dir_, number)));
  wal::Reader reader(std::move(file));
  std::string record;
  while (reader.ReadRecord(&record)) {
    CW_ASSIGN_OR_RETURN(WriteBatch batch, WriteBatch::FromContents(record));
    CW_RETURN_IF_ERROR(batch.InsertInto(mem_.get()));
    const SequenceNumber last = batch.Sequence() + batch.Count() - 1;
    if (last > versions_.last_sequence()) versions_.SetLastSequence(last);
  }
  CW_RETURN_IF_ERROR(reader.status());
  const auto& st = reader.stats();
  if (st.dropped_bytes > 0 || st.truncated_tail) {
    SPDLOG_WARN("lsm: WAL {:06d}: {} records, {} corrupt bytes skipped{}", number, st.records,
                st.dropped_bytes, st.truncated_tail ? ", torn tail ignored" : "");
  }
  static_cast<void>(edit);  // replayed data is flushed by Recover() into this edit
  return Status::Ok();
}

Status DB::Put(const WriteOptions& opts, std::string_view key, std::string_view value) {
  WriteBatch b;
  b.Put(key, value);
  return Write(opts, &b);
}

Status DB::Delete(const WriteOptions& opts, std::string_view key) {
  WriteBatch b;
  b.Delete(key);
  return Write(opts, &b);
}

WriteBatch* DB::BuildBatchGroup(Writer** last) {
  Writer* first = writers_.front();
  WriteBatch* result = first->batch;
  size_t size = result->ByteSize();
  // Small writes get a small group so they do not wait behind a big one.
  size_t max_size = options_.max_batch_group_bytes;
  if (size <= 128_KiB) max_size = std::min(max_size, size + 128_KiB);
  *last = first;
  for (auto it = writers_.begin() + 1; it != writers_.end(); ++it) {
    Writer* w = *it;
    if (w->batch == nullptr) break;      // a Flush request ends the group
    if (w->sync && !first->sync) break;  // never downgrade a sync write
    size += w->batch->ByteSize();
    if (size > max_size) break;
    if (result == first->batch) {
      group_batch_.Clear();
      group_batch_.Append(*first->batch);
      result = &group_batch_;
    }
    result->Append(*w->batch);
    *last = w;
  }
  return result;
}

Status DB::Write(const WriteOptions& opts, WriteBatch* batch) {
  Writer w;
  w.batch = batch;
  w.sync = opts.sync;
  std::unique_lock lock(mu_);
  writers_.push_back(&w);
  // Group commit: the writer at the head of the queue becomes the leader and
  // commits its followers' batches together with its own.
  while (!w.done && &w != writers_.front()) w.cv.wait(lock);
  if (w.done) return w.status;

  Status s = MakeRoomForWrite(lock, batch == nullptr);
  Writer* last = &w;
  if (s.ok() && batch != nullptr) {
    WriteBatch* group = BuildBatchGroup(&last);
    SequenceNumber seq = versions_.last_sequence();
    group->SetSequence(seq + 1);
    seq += group->Count();
    auto mem = mem_;
    wal::Writer* log = log_.get();
    size_t group_writers = 0;
    for (Writer* x : writers_) {
      ++group_writers;
      if (x == last) break;
    }
    const size_t group_bytes = group->ByteSize();
    {
      // WAL append, fsync, and memtable insert run without the mutex. Only
      // the leader is here, so the memtable still sees a single writer.
      lock.unlock();
      s = log->AddRecord(group->Contents());
      bool sync_failed = false;
      if (s.ok() && w.sync) {
        s = log->Sync();
        sync_failed = !s.ok();
      }
      if (s.ok()) s = group->InsertInto(mem.get());
      lock.lock();
      if (sync_failed) bg_error_ = s;  // the log tail is now unknown: refuse further writes
    }
    if (group == &group_batch_) group_batch_.Clear();
    if (s.ok()) {
      versions_.SetLastSequence(seq);  // publish: readers may now see the group
      stats_.write_groups += 1;
      stats_.writes += group_writers;
      stats_.bytes_written_user += group_bytes;
    }
  }

  for (;;) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = s;
      ready->done = true;
      ready->cv.notify_one();
    }
    if (ready == last) break;
  }
  if (!writers_.empty()) writers_.front()->cv.notify_one();
  return s;
}

Status DB::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  bool allow_delay = !force;
  for (;;) {
    CW_RETURN_IF_ERROR(bg_error_);
    const auto l0 = static_cast<int>(versions_.current()->files[0].size());
    if (allow_delay && l0 >= options_.l0_slowdown_trigger) {
      // Back-pressure: give compaction CPU instead of letting L0 grow until
      // a hard stop. A 1 ms delay per write spreads the stall over many
      // writes instead of one long pause.
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      lock.lock();
      allow_delay = false;
      stats_.slowdowns += 1;
      stats_.stall_micros += 1000;
    } else if (!force && mem_->ApproximateMemoryUsage() <= options_.write_buffer_size) {
      return Status::Ok();
    } else if (imm_ != nullptr || l0 >= options_.l0_stop_trigger) {
      const int64_t start = NowMicros();
      MaybeScheduleWork();
      done_cv_.wait(lock);
      stats_.stall_micros += static_cast<uint64_t>(NowMicros() - start);
    } else {
      if (force && mem_->entries() == 0) return Status::Ok();
      CW_RETURN_IF_ERROR(NewLogFile());
      imm_ = std::move(mem_);
      mem_ = std::make_shared<MemTable>();
      force = false;
      MaybeScheduleWork();
    }
  }
}

void DB::MaybeScheduleWork() {
  bg_cv_.notify_one();
}

void DB::BackgroundLoop() {
  std::unique_lock lock(mu_);
  for (;;) {
    bg_cv_.wait(lock, [this] {
      int level = 0;
      return shutting_down_ ||
             (bg_error_.ok() && (imm_ != nullptr || versions_.CompactionScore(&level) >= 1.0));
    });
    if (shutting_down_) break;  // unflushed data is safe in the WAL
    bg_busy_ = true;
    Status s;
    if (imm_ != nullptr) {
      s = FlushImmutable(lock);
    } else if (auto c = versions_.PickCompaction()) {
      s = RunCompaction(lock, std::move(*c));
    }
    if (!s.ok() && bg_error_.ok()) {
      bg_error_ = s;
      SPDLOG_ERROR("lsm: background error, writes are now refused: {}", s.ToString());
    }
    bg_busy_ = false;
    done_cv_.notify_all();
  }
  done_cv_.notify_all();
}

Status DB::WriteLevel0Table(std::unique_lock<std::mutex>& lock,
                            const std::shared_ptr<MemTable>& mem, VersionEdit* edit) {
  FileMetaData meta;
  meta.number = versions_.NewFileNumber();
  pending_outputs_.insert(meta.number);
  const bool held = lock.owns_lock();
  if (held) lock.unlock();
  auto it = MemTable::NewIterator(mem);
  Status s = BuildTable(env_, TableFileName(dir_, meta.number), options_.table, it.get(), &meta);
  if (held) lock.lock();
  pending_outputs_.erase(meta.number);
  if (s.ok() && meta.num_entries > 0) {
    stats_.bytes_written_flush += meta.file_size;
    edit->new_files.emplace_back(0, std::move(meta));
  }
  return s;
}

Status DB::FlushImmutable(std::unique_lock<std::mutex>& lock) {
  VersionEdit edit;
  std::shared_ptr<MemTable> imm = imm_;
  CW_RETURN_IF_ERROR(WriteLevel0Table(lock, imm, &edit));
  // Every log older than the current one is now covered by a table.
  edit.log_number = log_number_;
  CW_RETURN_IF_ERROR(versions_.LogAndApply(&edit));
  imm_.reset();
  stats_.flushes += 1;
  DeleteObsoleteFiles();
  if (options_.event_listener && !edit.new_files.empty()) {
    const auto& f = edit.new_files.front().second;
    const std::string event = fmt::format(
        "flush: memtable ({} entries) -> L0 #{:06d}.sst {:.1f} KiB   [{}]", imm->entries(),
        f.number, static_cast<double>(f.file_size) / 1024.0, versions_.current()->Summary());
    lock.unlock();
    options_.event_listener(event);
    lock.lock();
  }
  return Status::Ok();
}

Status DB::RunCompaction(std::unique_lock<std::mutex>& lock, Compaction c) {
  VersionEdit edit;
  if (c.IsTrivialMove()) {
    const FilePtr& f = c.inputs[0].front();
    edit.deleted_files.emplace(c.level, f->number);
    edit.new_files.emplace_back(c.level + 1, *f);
    CW_RETURN_IF_ERROR(versions_.LogAndApply(&edit));
    stats_.trivial_moves += 1;
    if (options_.event_listener) {
      const std::string event =
          fmt::format("trivial move: L{} #{:06d} -> L{} (no rewrite)   [{}]", c.level, f->number,
                      c.level + 1, versions_.current()->Summary());
      lock.unlock();
      options_.event_listener(event);
      lock.lock();
    }
    return Status::Ok();
  }

  std::vector<std::unique_ptr<InternalIterator>> children;
  uint64_t input_bytes = 0;
  for (int which = 0; which < 2; ++which) {
    for (const FilePtr& f : c.inputs[which]) {
      input_bytes += f->file_size;
      CW_ASSIGN_OR_RETURN(auto table, table_cache_.Find(f->number));
      children.push_back(Table::NewIterator(std::move(table)));
    }
  }
  auto input = NewMergingIterator(std::move(children));
  const int out_level = c.level + 1;

  std::vector<FileMetaData> outputs;
  std::unique_ptr<WritableFile> out_file;
  std::unique_ptr<TableBuilder> builder;
  Status s;

  auto finish_output = [&]() -> Status {
    if (!builder) return Status::Ok();
    CW_RETURN_IF_ERROR(builder->Finish());
    CW_RETURN_IF_ERROR(out_file->Sync());
    CW_RETURN_IF_ERROR(out_file->Close());
    outputs.back().file_size = builder->FileSize();
    outputs.back().num_entries = builder->num_entries();
    builder.reset();
    out_file.reset();
    return Status::Ok();
  };

  lock.unlock();
  std::string current_user_key;
  bool has_current = false;
  uint64_t dropped_shadowed = 0;
  uint64_t dropped_tombstones = 0;
  uint64_t kept = 0;
  for (input->SeekToFirst(); input->Valid() && s.ok(); input->Next()) {
    ParsedInternalKey ikey;
    if (!ParseInternalKey(input->key(), &ikey)) {
      s = Status::Corruption("compaction: bad internal key");
      break;
    }
    const bool first_version = !has_current || ikey.user_key != current_user_key;
    if (first_version) {
      current_user_key.assign(ikey.user_key);
      has_current = true;
    }
    // Older versions are shadowed by the newest one in this merge. Readers
    // that need them pinned the old Version, so its files stay alive. A
    // tombstone can go once no deeper level could still hold the key.
    if (!first_version) {
      ++dropped_shadowed;
      continue;
    }
    if (ikey.type == ValueType::kDeletion &&
        c.input_version->IsBaseLevelForKey(out_level, ikey.user_key)) {
      ++dropped_tombstones;
      continue;
    }
    ++kept;
    if (!builder) {
      lock.lock();
      FileMetaData meta;
      meta.number = versions_.NewFileNumber();
      pending_outputs_.insert(meta.number);
      lock.unlock();
      auto file = env_->NewWritableFile(TableFileName(dir_, meta.number));
      if (!file.ok()) {
        s = file.status();
        break;
      }
      out_file = std::move(file).value();
      builder = std::make_unique<TableBuilder>(options_.table, out_file.get());
      meta.smallest.assign(input->key());
      outputs.push_back(std::move(meta));
    }
    builder->Add(input->key(), input->value());
    outputs.back().largest.assign(input->key());
    if (builder->FileSize() >= options_.max_file_size) s = finish_output();
  }
  if (s.ok()) s = input->status();
  if (s.ok()) s = finish_output();
  lock.lock();

  if (s.ok()) {
    uint64_t output_bytes = 0;
    for (int which = 0; which < 2; ++which) {
      for (const FilePtr& f : c.inputs[which])
        edit.deleted_files.emplace(c.level + which, f->number);
    }
    for (auto& meta : outputs) {
      output_bytes += meta.file_size;
      edit.new_files.emplace_back(out_level, meta);
    }
    s = versions_.LogAndApply(&edit);
    stats_.compactions += 1;
    stats_.bytes_read_compaction += input_bytes;
    stats_.bytes_written_compaction += output_bytes;
    if (s.ok() && options_.event_listener) {
      const std::string event = fmt::format(
          "compaction: L{} ({} files) + L{} ({} files), {:.1f} KiB in -> L{} ({} files) {:.1f} KiB "
          "out; kept {} entries, dropped {} shadowed versions and {} tombstones   [{}]",
          c.level, c.inputs[0].size(), c.level + 1, c.inputs[1].size(),
          static_cast<double>(input_bytes) / 1024.0, out_level, outputs.size(),
          static_cast<double>(output_bytes) / 1024.0, kept, dropped_shadowed, dropped_tombstones,
          versions_.current()->Summary());
      lock.unlock();
      options_.event_listener(event);
      lock.lock();
    }
  }
  for (const auto& meta : outputs) pending_outputs_.erase(meta.number);
  DeleteObsoleteFiles();
  return s;
}

void DB::DeleteObsoleteFiles() {
  std::set<uint64_t> live = versions_.LiveFiles();
  live.insert(pending_outputs_.begin(), pending_outputs_.end());
  auto names = env_->ListDir(dir_);
  if (!names.ok()) return;
  for (const auto& name : *names) {
    uint64_t number = 0;
    bool keep = true;
    switch (ParseFileName(name, &number)) {
      case FileType::kLog: keep = number >= versions_.log_number() || number == log_number_; break;
      case FileType::kTable:
        keep = live.contains(number);
        if (!keep) table_cache_.Evict(number);
        break;
      case FileType::kManifest: keep = number >= versions_.manifest_number(); break;
      case FileType::kTemp: keep = false; break;
      default: break;
    }
    if (!keep) static_cast<void>(env_->RemoveFile(dir_ + "/" + name));
  }
}

Result<std::string> DB::Get(std::string_view key) {
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const Version> version;
  SequenceNumber seq = 0;
  {
    std::lock_guard lock(mu_);
    mem = mem_;
    imm = imm_;
    version = versions_.current();
    seq = versions_.last_sequence();
  }
  // Everything below runs lock-free on the pinned snapshot.
  const LookupKey lookup(key, seq);
  std::string value;
  for (const auto* m : {mem.get(), imm.get()}) {
    if (m == nullptr) continue;
    switch (m->Get(lookup, &value)) {
      case MemTable::GetResult::kFound: return value;
      case MemTable::GetResult::kDeleted: return Status::NotFound();
      case MemTable::GetResult::kNotFound: break;
    }
  }
  CW_ASSIGN_OR_RETURN(auto r, version->Get(lookup, &value, &table_cache_));
  if (r == MemTable::GetResult::kFound) return value;
  return Status::NotFound();
}

std::unique_ptr<DbIterator> DB::NewIterator() {
  std::shared_ptr<MemTable> mem;
  std::shared_ptr<MemTable> imm;
  std::shared_ptr<const Version> version;
  SequenceNumber seq = 0;
  {
    std::lock_guard lock(mu_);
    mem = mem_;
    imm = imm_;
    version = versions_.current();
    seq = versions_.last_sequence();
  }
  std::vector<std::unique_ptr<InternalIterator>> children;
  children.push_back(MemTable::NewIterator(mem));
  if (imm) children.push_back(MemTable::NewIterator(imm));
  version->AddIterators(&table_cache_, &children);
  auto pin = std::make_shared<std::tuple<std::shared_ptr<MemTable>, std::shared_ptr<MemTable>,
                                         std::shared_ptr<const Version>>>(mem, imm, version);
  return std::make_unique<DbIterator>(NewMergingIterator(std::move(children)), seq, pin);
}

Status DB::Flush() {
  CW_RETURN_IF_ERROR(Write(WriteOptions{}, nullptr));
  std::unique_lock lock(mu_);
  done_cv_.wait(lock, [this] { return imm_ == nullptr || !bg_error_.ok(); });
  return bg_error_;
}

Status DB::WaitForCompactions() {
  std::unique_lock lock(mu_);
  for (;;) {
    int level = 0;
    if (!bg_error_.ok()) return bg_error_;
    if (imm_ == nullptr && !bg_busy_ && versions_.CompactionScore(&level) < 1.0)
      return Status::Ok();
    MaybeScheduleWork();
    done_cv_.wait(lock);
  }
}

DbStats DB::Stats() const {
  std::lock_guard lock(mu_);
  DbStats s = stats_;
  const auto v = versions_.current();
  s.levels = v->Summary();
  for (int l = 0; l < kNumLevels; ++l) {
    s.level_bytes[static_cast<size_t>(l)] = v->LevelBytes(l);
    s.level_files[static_cast<size_t>(l)] = v->files[static_cast<size_t>(l)].size();
  }
  s.memtable_bytes = mem_->ApproximateMemoryUsage() + (imm_ ? imm_->ApproximateMemoryUsage() : 0);
  s.memtable_entries = mem_->entries() + (imm_ ? imm_->entries() : 0);
  for (const auto& level_files : v->files) {
    for (const FilePtr& f : level_files) s.table_entries += f->num_entries;
  }
  s.block_cache_hits = block_cache_.hits();
  s.block_cache_misses = block_cache_.misses();
  return s;
}

// ---- storage::Engine adapter ---------------------------------------------------------

namespace {

class LsmIterator final : public Iterator {
 public:
  explicit LsmIterator(std::unique_ptr<DbIterator> it) : it_(std::move(it)) {}
  bool Valid() const override { return it_->Valid(); }
  void SeekToFirst() override { it_->SeekToFirst(); }
  void Seek(std::string_view target) override { it_->Seek(target); }
  void Next() override { it_->Next(); }
  std::string_view key() const override { return it_->key(); }
  std::string_view value() const override { return it_->value(); }
  Status status() const override { return it_->status(); }

 private:
  std::unique_ptr<DbIterator> it_;
};

class LsmEngine final : public Engine {
 public:
  explicit LsmEngine(std::unique_ptr<DB> db) : db_(std::move(db)) {}

  Status Put(std::string_view key, std::string_view value, const WriteOptions& opts) override {
    return db_->Put(opts, key, value);
  }
  Status Delete(std::string_view key, const WriteOptions& opts) override {
    return db_->Delete(opts, key);
  }
  Result<std::string> Get(std::string_view key) const override { return db_->Get(key); }
  std::unique_ptr<Iterator> NewIterator() const override {
    return std::make_unique<LsmIterator>(db_->NewIterator());
  }
  EngineStats Stats() const override {
    const DbStats s = db_->Stats();
    EngineStats out;
    for (const uint64_t b : s.level_bytes) out.bytes += b;
    out.bytes += s.memtable_bytes;
    // Upper bound: entries across tables and memtables, counting overwritten
    // versions and tombstones not yet compacted away. Exact counts need a scan.
    out.keys = s.table_entries + s.memtable_entries;
    return out;
  }
  std::string_view Name() const override { return "lsm"; }

 private:
  std::unique_ptr<DB> db_;
};

}  // namespace

Result<std::unique_ptr<Engine>> NewLsmEngine(const Options& options, const std::string& dir) {
  CW_ASSIGN_OR_RETURN(auto db, DB::Open(options, dir));
  return std::unique_ptr<Engine>(std::make_unique<LsmEngine>(std::move(db)));
}

}  // namespace cartwheel::storage::lsm
