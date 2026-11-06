#include "common/logging.h"

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

TEST(LoggingTest, JsonEscapeHandlesSpecials) {
  EXPECT_EQ(JsonEscape("plain"), "plain");
  EXPECT_EQ(JsonEscape("say \"hi\""), "say \\\"hi\\\"");
  EXPECT_EQ(JsonEscape("a\\b"), "a\\\\b");
  EXPECT_EQ(JsonEscape("line1\nline2\t"), "line1\\nline2\\t");
  EXPECT_EQ(JsonEscape(std::string_view("\x01", 1)), "\\u0001");
}

TEST(LoggingTest, InitInstallsDefaultLogger) {
  InitLogging({.service = "logging-test", .level = "debug", .json = true});
  EXPECT_EQ(spdlog::default_logger()->name(), "logging-test");
  EXPECT_EQ(spdlog::default_logger()->level(), spdlog::level::debug);
  CW_LOG_INFO("json line with \"quotes\"");
  spdlog::drop("logging-test");
}

}  // namespace
}  // namespace cartwheel
