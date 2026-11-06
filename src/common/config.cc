#include "common/config.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <sstream>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

namespace cartwheel {

struct Config::Tree {
  toml::table table;
};

namespace {

std::optional<int64_t> ParseInt(const std::string& s) {
  if (s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size()) return std::nullopt;
  return static_cast<int64_t>(v);
}

std::optional<double> ParseDouble(const std::string& s) {
  if (s.empty()) return std::nullopt;
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(s.c_str(), &end);
  if (errno != 0 || end != s.c_str() + s.size()) return std::nullopt;
  return v;
}

std::optional<bool> ParseBool(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
  if (s == "0" || s == "false" || s == "no" || s == "off") return false;
  return std::nullopt;
}

void WarnBadEnv(const std::string& var, const std::string& value, std::string_view type) {
  spdlog::warn("ignoring env {}={:?}: not a valid {}", var, value, type);
}

}  // namespace

Config::Config() : tree_(std::make_shared<Tree>()), env_prefix_("CARTWHEEL") {}

Result<Config> Config::FromString(std::string_view toml_text, std::string env_prefix) {
  try {
    Config cfg;
    cfg.tree_ = std::make_shared<Tree>(Tree{toml::parse(toml_text)});
    cfg.env_prefix_ = std::move(env_prefix);
    return cfg;
  } catch (const toml::parse_error& e) {
    std::ostringstream msg;
    msg << "config parse error at line " << e.source().begin.line << ": " << e.description();
    return Status::InvalidArgument(msg.str());
  }
}

Result<Config> Config::FromFile(const std::string& path, std::string env_prefix) {
  try {
    Config cfg;
    cfg.tree_ = std::make_shared<Tree>(Tree{toml::parse_file(path)});
    cfg.env_prefix_ = std::move(env_prefix);
    return cfg;
  } catch (const toml::parse_error& e) {
    std::ostringstream msg;
    msg << path << ":" << e.source().begin.line << ": " << e.description();
    return Status::InvalidArgument(msg.str());
  }
}

std::string Config::EnvVarName(std::string_view path) const {
  std::string name = env_prefix_;
  name += '_';
  for (const char c : path) {
    name += (c == '.' || c == '-') ? '_'
                                   : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return name;
}

std::optional<std::string> Config::EnvOverride(std::string_view path) const {
  const std::string var = EnvVarName(path);
  const char* v = std::getenv(var.c_str());  // NOLINT(concurrency-mt-unsafe): read at startup
  if (v == nullptr) return std::nullopt;
  return std::string(v);
}

bool Config::Has(std::string_view path) const {
  return EnvOverride(path).has_value() || static_cast<bool>(tree_->table.at_path(path));
}

std::string Config::GetString(std::string_view path, std::string_view fallback) const {
  if (auto env = EnvOverride(path)) return *env;
  if (auto v = tree_->table.at_path(path).value<std::string>()) return *v;
  return std::string(fallback);
}

int64_t Config::GetInt(std::string_view path, int64_t fallback) const {
  if (auto env = EnvOverride(path)) {
    if (auto parsed = ParseInt(*env)) return *parsed;
    WarnBadEnv(EnvVarName(path), *env, "integer");
  }
  if (auto v = tree_->table.at_path(path).value<int64_t>()) return *v;
  return fallback;
}

double Config::GetDouble(std::string_view path, double fallback) const {
  if (auto env = EnvOverride(path)) {
    if (auto parsed = ParseDouble(*env)) return *parsed;
    WarnBadEnv(EnvVarName(path), *env, "number");
  }
  // value<double>() also converts TOML integers, so "rate = 100" works.
  if (auto v = tree_->table.at_path(path).value<double>()) return *v;
  return fallback;
}

bool Config::GetBool(std::string_view path, bool fallback) const {
  if (auto env = EnvOverride(path)) {
    if (auto parsed = ParseBool(*env)) return *parsed;
    WarnBadEnv(EnvVarName(path), *env, "boolean");
  }
  if (auto v = tree_->table.at_path(path).value<bool>()) return *v;
  return fallback;
}

std::vector<std::string> Config::GetStringList(std::string_view path) const {
  std::vector<std::string> out;
  if (auto env = EnvOverride(path)) {
    std::stringstream ss(*env);
    std::string item;
    while (std::getline(ss, item, ',')) {
      if (!item.empty()) out.push_back(item);
    }
    return out;
  }
  if (const auto* arr = tree_->table.at_path(path).as_array()) {
    for (const auto& node : *arr) {
      if (auto s = node.value<std::string>()) out.push_back(*s);
    }
  }
  return out;
}

Result<std::string> Config::RequireString(std::string_view path) const {
  if (auto env = EnvOverride(path)) return *env;
  auto node = tree_->table.at_path(path);
  if (!node) return Status::NotFound("missing required config key '" + std::string(path) + "'");
  if (auto v = node.value<std::string>()) return *v;
  return Status::InvalidArgument("config key '" + std::string(path) + "' must be a string");
}

Result<int64_t> Config::RequireInt(std::string_view path) const {
  if (auto env = EnvOverride(path)) {
    if (auto parsed = ParseInt(*env)) return *parsed;
    return Status::InvalidArgument(EnvVarName(path) + " must be an integer");
  }
  auto node = tree_->table.at_path(path);
  if (!node) return Status::NotFound("missing required config key '" + std::string(path) + "'");
  if (auto v = node.value<int64_t>()) return *v;
  return Status::InvalidArgument("config key '" + std::string(path) + "' must be an integer");
}

}  // namespace cartwheel
