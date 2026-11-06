#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "common/units.h"
#include "storage/env.h"

namespace cartwheel::storage {
namespace {

Status PosixError(const std::string& context, int err) {
  std::string msg = context + ": " + std::error_code(err, std::generic_category()).message();
  if (err == ENOENT) return Status::NotFound(msg);
  return Status::IOError(msg);
}

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(std::string path, int fd, uint64_t size)
      : path_(std::move(path)), fd_(fd), size_(size) {}
  ~PosixWritableFile() override {
    if (fd_ >= 0) static_cast<void>(Close());
  }

  Status Append(std::string_view data) override {
    // Small user-space buffer: WAL records are often tiny, and one write(2)
    // per record is the dominant cost without it.
    if (buf_.size() + data.size() > kBufferSize) CW_RETURN_IF_ERROR(Flush());
    if (data.size() > kBufferSize) return WriteRaw(data);
    buf_.append(data);
    size_ += data.size();
    return Status::Ok();
  }

  Status Flush() override {
    if (buf_.empty()) return Status::Ok();
    const std::string pending = std::move(buf_);
    buf_.clear();
    size_ -= pending.size();
    return WriteRaw(pending);
  }

  Status Sync() override {
    CW_RETURN_IF_ERROR(Flush());
#if defined(__APPLE__)
    // macOS fsync() does not flush the drive cache; F_FULLFSYNC does.
    if (::fcntl(fd_, F_FULLFSYNC) == 0) return Status::Ok();
#endif
#if defined(__linux__)
    if (::fdatasync(fd_) != 0) return PosixError(path_, errno);
#else
    if (::fsync(fd_) != 0) return PosixError(path_, errno);
#endif
    return Status::Ok();
  }

  Status Close() override {
    Status s = Flush();
    if (fd_ >= 0 && ::close(fd_) != 0 && s.ok()) s = PosixError(path_, errno);
    fd_ = -1;
    return s;
  }

  uint64_t Size() const override { return size_; }

 private:
  static constexpr size_t kBufferSize = 64_KiB;

  Status WriteRaw(std::string_view data) {
    while (!data.empty()) {
      const ssize_t n = ::write(fd_, data.data(), data.size());
      if (n < 0) {
        if (errno == EINTR) continue;
        return PosixError(path_, errno);
      }
      data.remove_prefix(static_cast<size_t>(n));
      size_ += static_cast<uint64_t>(n);
    }
    return Status::Ok();
  }

  std::string path_;
  int fd_;
  uint64_t size_;
  std::string buf_;
};

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(std::string path, int fd) : path_(std::move(path)), fd_(fd) {}
  ~PosixSequentialFile() override { ::close(fd_); }

  Status Read(size_t n, std::string_view* result, char* scratch) override {
    size_t got = 0;
    while (got < n) {
      const ssize_t r = ::read(fd_, scratch + got, n - got);
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(path_, errno);
      }
      if (r == 0) break;
      got += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, got);
    return Status::Ok();
  }

  Status Skip(uint64_t n) override {
    if (::lseek(fd_, static_cast<off_t>(n), SEEK_CUR) < 0) return PosixError(path_, errno);
    return Status::Ok();
  }

 private:
  std::string path_;
  int fd_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string path, int fd, uint64_t size)
      : path_(std::move(path)), fd_(fd), size_(size) {}
  ~PosixRandomAccessFile() override { ::close(fd_); }

  Status Read(uint64_t offset, size_t n, std::string_view* result, char* scratch) const override {
    size_t got = 0;
    while (got < n) {
      const ssize_t r = ::pread(fd_, scratch + got, n - got, static_cast<off_t>(offset + got));
      if (r < 0) {
        if (errno == EINTR) continue;
        return PosixError(path_, errno);
      }
      if (r == 0) break;
      got += static_cast<size_t>(r);
    }
    *result = std::string_view(scratch, got);
    return Status::Ok();
  }

  uint64_t Size() const override { return size_; }

 private:
  std::string path_;
  int fd_;
  uint64_t size_;
};

class PosixFileLock final : public FileLock {
 public:
  explicit PosixFileLock(int fd) : fd_(fd) {}
  ~PosixFileLock() override {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }

 private:
  int fd_;
};

class PosixEnv final : public Env {
 public:
  Result<std::unique_ptr<WritableFile>> NewWritableFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    return std::unique_ptr<WritableFile>(std::make_unique<PosixWritableFile>(path, fd, 0));
  }

  Result<std::unique_ptr<WritableFile>> NewAppendableFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      const int err = errno;
      ::close(fd);
      return PosixError(path, err);
    }
    return std::unique_ptr<WritableFile>(
        std::make_unique<PosixWritableFile>(path, fd, static_cast<uint64_t>(st.st_size)));
  }

  Result<std::unique_ptr<SequentialFile>> NewSequentialFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    return std::unique_ptr<SequentialFile>(std::make_unique<PosixSequentialFile>(path, fd));
  }

  Result<std::unique_ptr<RandomAccessFile>> NewRandomAccessFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(path, errno);
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      const int err = errno;
      ::close(fd);
      return PosixError(path, err);
    }
    return std::unique_ptr<RandomAccessFile>(
        std::make_unique<PosixRandomAccessFile>(path, fd, static_cast<uint64_t>(st.st_size)));
  }

  bool FileExists(const std::string& path) override { return ::access(path.c_str(), F_OK) == 0; }

  Result<std::vector<std::string>> ListDir(const std::string& dir) override {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return PosixError(dir, errno);
    std::vector<std::string> out;
    // readdir is safe here: this DIR* is private to the call.
    while (const dirent* e = ::readdir(d)) {  // NOLINT(concurrency-mt-unsafe)
      const std::string name = e->d_name;
      if (name != "." && name != "..") out.push_back(name);
    }
    ::closedir(d);
    return out;
  }

  Result<uint64_t> FileSize(const std::string& path) override {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return PosixError(path, errno);
    return static_cast<uint64_t>(st.st_size);
  }

  Status RemoveFile(const std::string& path) override {
    if (::unlink(path.c_str()) != 0) return PosixError(path, errno);
    return Status::Ok();
  }

  Status RenameFile(const std::string& from, const std::string& to) override {
    if (::rename(from.c_str(), to.c_str()) != 0) return PosixError(from + " -> " + to, errno);
    return Status::Ok();
  }

  Status CreateDirIfMissing(const std::string& dir) override {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return PosixError(dir, errno);
    return Status::Ok();
  }

  Status SyncDir(const std::string& dir) override {
    const int fd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return PosixError(dir, errno);
    const int rc = ::fsync(fd);
    const int err = errno;
    ::close(fd);
    if (rc != 0) return PosixError(dir, err);
    return Status::Ok();
  }

  Result<std::unique_ptr<FileLock>> LockFile(const std::string& path) override {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return PosixError(path, errno);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd);
      return Status::FailedPrecondition(path + " is locked by another process");
    }
    return std::unique_ptr<FileLock>(std::make_unique<PosixFileLock>(fd));
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv env;
  return &env;
}

Status WriteFileAtomically(Env* env, const std::string& path, std::string_view data) {
  const std::string tmp = path + ".tmp";
  {
    CW_ASSIGN_OR_RETURN(auto f, env->NewWritableFile(tmp));
    CW_RETURN_IF_ERROR(f->Append(data));
    CW_RETURN_IF_ERROR(f->Sync());
    CW_RETURN_IF_ERROR(f->Close());
  }
  CW_RETURN_IF_ERROR(env->RenameFile(tmp, path));
  const auto slash = path.find_last_of('/');
  return env->SyncDir(slash == std::string::npos ? "." : path.substr(0, slash));
}

Result<std::string> ReadFileToString(Env* env, const std::string& path) {
  CW_ASSIGN_OR_RETURN(auto f, env->NewSequentialFile(path));
  std::string out;
  std::string scratch(64_KiB, '\0');
  for (;;) {
    std::string_view chunk;
    CW_RETURN_IF_ERROR(f->Read(scratch.size(), &chunk, scratch.data()));
    if (chunk.empty()) break;
    out.append(chunk);
  }
  return out;
}

}  // namespace cartwheel::storage
