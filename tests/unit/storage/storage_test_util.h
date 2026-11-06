#pragma once

#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>

namespace cartwheel::storage::testing {

// A fresh directory under the system temp dir, removed on destruction.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "cartwheel-XXXXXX").string();
    char* p = ::mkdtemp(tmpl.data());
    path_ = p != nullptr ? std::string(p) : tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string File(const std::string& name) const { return path_ + "/" + name; }

 private:
  std::string path_;
};

inline std::string RandomString(std::mt19937_64& rng, size_t len) {
  std::string s(len, '\0');
  for (auto& c : s) c = static_cast<char>('a' + rng() % 26);
  return s;
}

}  // namespace cartwheel::storage::testing
