#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace cartwheel::storage {

// File-system seam for the storage engine. Production uses PosixEnv. Crash
// tests wrap it in FaultInjectionEnv, which can "lose power" by dropping
// every byte written since the last Sync(). Durability claims are tested
// against what a real crash keeps, not against what the process wrote.

class WritableFile {
 public:
  virtual ~WritableFile() = default;
  virtual Status Append(std::string_view data) = 0;
  virtual Status Flush() = 0;  // to the OS page cache
  virtual Status Sync() = 0;   // to stable storage (fdatasync / F_FULLFSYNC)
  virtual Status Close() = 0;
  [[nodiscard]] virtual uint64_t Size() const = 0;
};

class SequentialFile {
 public:
  virtual ~SequentialFile() = default;
  // Reads up to n bytes into scratch; *result points into scratch. A short
  // read with OK status means EOF.
  virtual Status Read(size_t n, std::string_view* result, char* scratch) = 0;
  virtual Status Skip(uint64_t n) = 0;
};

class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;
  // Thread-safe positional read (pread).
  virtual Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const = 0;
  [[nodiscard]] virtual uint64_t Size() const = 0;
};

class FileLock {
 public:
  virtual ~FileLock() = default;
};

class Env {
 public:
  virtual ~Env() = default;

  virtual Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) = 0;
  virtual Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) = 0;
  virtual Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& path) = 0;
  virtual Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(
      const std::string& path) = 0;

  [[nodiscard]] virtual bool FileExists(const std::string& path) = 0;
  virtual Result<std::vector<std::string>> ListDir(const std::string& dir) = 0;
  virtual Result<uint64_t> FileSize(const std::string& path) = 0;
  virtual Status RemoveFile(const std::string& path) = 0;
  virtual Status RenameFile(const std::string& from, const std::string& to) = 0;
  virtual Status CreateDirIfMissing(const std::string& dir) = 0;
  // fsync a directory so a create/rename inside it is durable.
  virtual Status SyncDir(const std::string& dir) = 0;
  // Advisory exclusive lock: two processes must never open one DB directory.
  virtual Result<std::unique_ptr<FileLock>> LockFile(const std::string& path) = 0;

  static Env* Default();  // PosixEnv singleton
};

// Writes `data` to `path` via a temp file + fsync + rename + dir fsync.
// Either the old or the new contents survive a crash, never a mix.
Status WriteFileAtomically(Env* env, const std::string& path, std::string_view data);
Result<std::string> ReadFileToString(Env* env, const std::string& path);

}  // namespace cartwheel::storage
