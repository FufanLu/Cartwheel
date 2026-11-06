#pragma once

#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace cartwheel {

struct LogOptions {
  std::string service = "cartwheel";
  std::string level = "info";  // trace|debug|info|warn|error|critical|off
  bool json = false;           // one JSON object per line (for Loki/CloudWatch)
};

// Installs the process-wide default spdlog logger writing to stdout.
// Call once at startup, before spawning threads.
void InitLogging(const LogOptions& options);

// Escapes a string for embedding inside a JSON string literal.
[[nodiscard]] std::string JsonEscape(std::string_view in);

}  // namespace cartwheel

// Thin aliases so call sites read uniformly and can be re-pointed later.
#define CW_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define CW_LOG_DEBUG(...) SPDLOG_DEBUG(__VA_ARGS__)
#define CW_LOG_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#define CW_LOG_WARN(...) SPDLOG_WARN(__VA_ARGS__)
#define CW_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)
