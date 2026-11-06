#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace cartwheel {

// Layered configuration: environment > TOML file > compiled-in default.
//
// Keys are dotted paths ("server.port"). Each key can be overridden by an
// environment variable named PREFIX_SECTION_KEY, upper-cased, with dots
// becoming underscores: CARTWHEEL_SERVER_PORT=9000. Containers and Helm
// charts configure services through env vars, so no files need templating.
class Config {
 public:
  Config();  // empty config: every Get returns env override or fallback

  static Result<Config> FromFile(const std::string& path, std::string env_prefix = "CARTWHEEL");
  static Result<Config> FromString(std::string_view toml, std::string env_prefix = "CARTWHEEL");

  [[nodiscard]] std::string GetString(std::string_view path, std::string_view fallback) const;
  [[nodiscard]] int64_t GetInt(std::string_view path, int64_t fallback) const;
  [[nodiscard]] double GetDouble(std::string_view path, double fallback) const;
  [[nodiscard]] bool GetBool(std::string_view path, bool fallback) const;
  // TOML array of strings, or a comma-separated env var ("a:1,b:2").
  [[nodiscard]] std::vector<std::string> GetStringList(std::string_view path) const;

  // Required keys: NotFound if missing, InvalidArgument if the wrong type.
  [[nodiscard]] Result<std::string> RequireString(std::string_view path) const;
  [[nodiscard]] Result<int64_t> RequireInt(std::string_view path) const;

  [[nodiscard]] bool Has(std::string_view path) const;

  // "server.port" -> "CARTWHEEL_SERVER_PORT"
  [[nodiscard]] std::string EnvVarName(std::string_view path) const;

 private:
  [[nodiscard]] std::optional<std::string> EnvOverride(std::string_view path) const;

  struct Tree;  // wraps toml::table; keeps toml++ out of this header
  std::shared_ptr<const Tree> tree_;
  std::string env_prefix_;
};

}  // namespace cartwheel
