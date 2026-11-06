#include "common/config.h"

#include <cstdlib>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

constexpr const char* kToml = R"(
[server]
port = 7000
host = "0.0.0.0"
ratio = 0.75
tls = false

[cluster]
seeds = ["wheel-1:7100", "wheel-2:7100"]
replication = { n = 3, r = 2, w = 2 }
)";

// Scoped env var for override tests (tests run single-threaded per process).
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    setenv(name, value, 1);  // NOLINT(concurrency-mt-unsafe): gtest runs tests serially
  }
  ~ScopedEnv() { unsetenv(name_); }  // NOLINT(concurrency-mt-unsafe)
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
};

TEST(ConfigTest, ReadsTypedValuesByDottedPath) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok()) << cfg.status();
  EXPECT_EQ(cfg->GetInt("server.port", 0), 7000);
  EXPECT_EQ(cfg->GetString("server.host", ""), "0.0.0.0");
  EXPECT_DOUBLE_EQ(cfg->GetDouble("server.ratio", 0), 0.75);
  EXPECT_FALSE(cfg->GetBool("server.tls", true));
  EXPECT_EQ(cfg->GetInt("cluster.replication.w", 0), 2);
  EXPECT_EQ(cfg->GetStringList("cluster.seeds"),
            (std::vector<std::string>{"wheel-1:7100", "wheel-2:7100"}));
}

TEST(ConfigTest, FallbackWhenMissing) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok());
  EXPECT_EQ(cfg->GetInt("server.missing", 42), 42);
  EXPECT_EQ(cfg->GetString("nope.nope", "dflt"), "dflt");
  EXPECT_FALSE(cfg->Has("server.missing"));
  EXPECT_TRUE(cfg->Has("server.port"));
}

TEST(ConfigTest, IntegerUsableAsDouble) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok());
  EXPECT_DOUBLE_EQ(cfg->GetDouble("server.port", 0), 7000.0);
}

TEST(ConfigTest, EnvOverridesFile) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok());
  EXPECT_EQ(cfg->EnvVarName("cluster.replication.w"), "CWTEST_CLUSTER_REPLICATION_W");

  const ScopedEnv port("CWTEST_SERVER_PORT", "9001");
  const ScopedEnv tls("CWTEST_SERVER_TLS", "true");
  const ScopedEnv seeds("CWTEST_CLUSTER_SEEDS", "a:1,b:2,c:3");
  EXPECT_EQ(cfg->GetInt("server.port", 0), 9001);
  EXPECT_TRUE(cfg->GetBool("server.tls", false));
  EXPECT_EQ(cfg->GetStringList("cluster.seeds"), (std::vector<std::string>{"a:1", "b:2", "c:3"}));
}

TEST(ConfigTest, MalformedEnvFallsBackToFile) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok());
  const ScopedEnv port("CWTEST_SERVER_PORT", "90o1");
  EXPECT_EQ(cfg->GetInt("server.port", 0), 7000);
  EXPECT_EQ(cfg->RequireInt("server.port").status().code(), StatusCode::kInvalidArgument);
}

TEST(ConfigTest, RequireReportsMissingAndWrongType) {
  auto cfg = Config::FromString(kToml, "CWTEST");
  ASSERT_TRUE(cfg.ok());
  EXPECT_EQ(cfg->RequireString("server.host").value(), "0.0.0.0");
  EXPECT_EQ(cfg->RequireInt("server.absent").status().code(), StatusCode::kNotFound);
  EXPECT_EQ(cfg->RequireInt("server.host").status().code(), StatusCode::kInvalidArgument);
}

TEST(ConfigTest, ParseErrorIsInvalidArgument) {
  auto cfg = Config::FromString("[server\nport = ", "CWTEST");
  ASSERT_FALSE(cfg.ok());
  EXPECT_EQ(cfg.status().code(), StatusCode::kInvalidArgument);
  EXPECT_NE(cfg.status().message().find("line"), std::string::npos);
}

TEST(ConfigTest, MissingFile) {
  auto cfg = Config::FromFile("/nonexistent/cartwheel.toml");
  EXPECT_FALSE(cfg.ok());
}

}  // namespace
}  // namespace cartwheel
