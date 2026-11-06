#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unistd.h>

#include "storage/env.h"

namespace cartwheel::storage::testing {

// Wraps the real Env and remembers, for every file written through it, how
// many bytes had been fsync'ed. DropUnsyncedData() truncates each file back
// to that point, which is what a power cut leaves on disk. Durability tests
// "crash" with it, then reopen the DB on the surviving bytes.
//
// It does not model lost directory entries, which needs dir-fsync tracking,
// or reordered writes inside the page cache.
class FaultInjectionEnv final : public Env {
 public:
  explicit FaultInjectionEnv(Env* base) : base_(base) {}

  void DropUnsyncedData() {
    std::lock_guard lock(mu_);
    for (const auto& [path, synced] : synced_) {
      if (base_->FileExists(path))
        static_cast<void>(::truncate(path.c_str(), static_cast<off_t>(synced)));
    }
  }

  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    CW_ASSIGN_OR_RETURN(auto f, base_->NewWritableFile(path));
    Track(path, 0);
    return std::unique_ptr<WritableFile>(std::make_unique<File>(this, path, std::move(f)));
  }
  Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) override {
    CW_ASSIGN_OR_RETURN(auto f, base_->NewAppendableFile(path));
    Track(path, f->Size());
    return std::unique_ptr<WritableFile>(std::make_unique<File>(this, path, std::move(f)));
  }
  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& p) override {
    return base_->NewSequentialFile(p);
  }
  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(const std::string& p) override {
    return base_->NewRandomAccessFile(p);
  }
  bool FileExists(const std::string& p) override { return base_->FileExists(p); }
  Result<std::vector<std::string>> ListDir(const std::string& d) override {
    return base_->ListDir(d);
  }
  Result<uint64_t> FileSize(const std::string& p) override { return base_->FileSize(p); }
  Status RemoveFile(const std::string& p) override {
    {
      std::lock_guard lock(mu_);
      synced_.erase(p);
    }
    return base_->RemoveFile(p);
  }
  Status RenameFile(const std::string& from, const std::string& to) override {
    {
      std::lock_guard lock(mu_);
      if (auto it = synced_.find(from); it != synced_.end()) {
        synced_[to] = it->second;
        synced_.erase(it);
      }
    }
    return base_->RenameFile(from, to);
  }
  Status CreateDirIfMissing(const std::string& d) override { return base_->CreateDirIfMissing(d); }
  Status SyncDir(const std::string& d) override { return base_->SyncDir(d); }
  Result<std::unique_ptr<FileLock>> LockFile(const std::string& p) override {
    return base_->LockFile(p);
  }

 private:
  class File final : public WritableFile {
   public:
    File(FaultInjectionEnv* env, std::string path, std::unique_ptr<WritableFile> base)
        : env_(env), path_(std::move(path)), base_(std::move(base)) {}
    Status Append(std::string_view d) override { return base_->Append(d); }
    Status Flush() override { return base_->Flush(); }
    Status Sync() override {
      CW_RETURN_IF_ERROR(base_->Sync());
      env_->Track(path_, base_->Size());
      return Status::Ok();
    }
    Status Close() override { return base_->Close(); }
    uint64_t Size() const override { return base_->Size(); }

   private:
    FaultInjectionEnv* env_;
    std::string path_;
    std::unique_ptr<WritableFile> base_;
  };

  void Track(const std::string& path, uint64_t synced) {
    std::lock_guard lock(mu_);
    synced_[path] = synced;
  }

  Env* base_;
  std::mutex mu_;
  std::map<std::string, uint64_t> synced_;
};

}  // namespace cartwheel::storage::testing
