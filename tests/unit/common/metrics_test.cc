#include "common/metrics.h"

#include <gtest/gtest.h>
#include <prometheus/counter.h>

namespace cartwheel {
namespace {

TEST(MetricsTest, SerializesRegisteredFamilies) {
  prometheus::Registry registry;
  auto& requests = prometheus::BuildCounter()
                       .Name("cartwheel_test_requests_total")
                       .Help("test counter")
                       .Register(registry);
  requests.Add({{"op", "put"}}).Increment(3);
  RegisterBuildInfo(registry, "metrics-test");

  const std::string text = SerializeMetrics(registry);
  EXPECT_NE(text.find(R"(cartwheel_test_requests_total{op="put"} 3)"), std::string::npos) << text;
  EXPECT_NE(text.find("cartwheel_build_info{"), std::string::npos);
  EXPECT_NE(text.find(R"(service="metrics-test")"), std::string::npos);
}

TEST(MetricsTest, LatencyBucketsAreIncreasing) {
  const auto b = LatencyBucketsSeconds();
  ASSERT_GE(b.size(), 15U);
  EXPECT_DOUBLE_EQ(b.front(), 50e-6);
  for (size_t i = 1; i < b.size(); ++i) EXPECT_GT(b[i], b[i - 1]);
}

TEST(MetricsTest, ServerBindsEphemeralPort) {
  auto server = MetricsServer::Start("127.0.0.1:0", std::make_shared<prometheus::Registry>());
  ASSERT_TRUE(server.ok()) << server.status();
  EXPECT_GT((*server)->port(), 0);
}

}  // namespace
}  // namespace cartwheel
