#include "common/logging.h"

#include <memory>

#include <fmt/format.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace cartwheel {
namespace {

void AppendJsonEscaped(std::string_view in, spdlog::memory_buf_t& out) {
  for (const char c : in) {
    switch (c) {
      case '"': out.append(std::string_view("\\\"")); break;
      case '\\': out.append(std::string_view("\\\\")); break;
      case '\n': out.append(std::string_view("\\n")); break;
      case '\r': out.append(std::string_view("\\r")); break;
      case '\t': out.append(std::string_view("\\t")); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out.push_back(c);
        }
    }
  }
}

// %* in a pattern: the log message, JSON-escaped.
class JsonMessageFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& msg, const std::tm& /*tm*/,
              spdlog::memory_buf_t& dest) override {
    AppendJsonEscaped(std::string_view(msg.payload.data(), msg.payload.size()), dest);
  }
  [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<JsonMessageFlag>();
  }
};

}  // namespace

std::string JsonEscape(std::string_view in) {
  spdlog::memory_buf_t buf;
  AppendJsonEscaped(in, buf);
  return {buf.data(), buf.size()};
}

void InitLogging(const LogOptions& options) {
  auto logger = spdlog::stdout_color_mt(options.service);
  auto formatter = std::make_unique<spdlog::pattern_formatter>();
  if (options.json) {
    // Literal service name is escaped once here; the message is escaped per line by %*.
    formatter->add_flag<JsonMessageFlag>('*').set_pattern(fmt::format(
        R"({{"ts":"%Y-%m-%dT%H:%M:%S.%f%z","level":"%l","service":"{}","tid":%t,"src":"%s:%#","msg":"%*"}})",
        JsonEscape(options.service)));
  } else {
    formatter->set_pattern("%Y-%m-%d %H:%M:%S.%e %^%-5l%$ [%n] [%t] %v");
  }
  logger->set_formatter(std::move(formatter));
  logger->set_level(spdlog::level::from_str(options.level));
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(std::move(logger));
}

}  // namespace cartwheel
