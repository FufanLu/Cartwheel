#include "common/tracing.h"

#include <gtest/gtest.h>

namespace cartwheel::tracing {
namespace {

TEST(Tracing, DisabledSpansAreFreeNullObjects) {
  ASSERT_FALSE(Enabled());
  Span s =
      StartSpan("noop", Kind::kServer, "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01");
  EXPECT_FALSE(s);
  s.SetAttribute("k", "v");
  s.SetAttribute("n", int64_t{1});
  s.SetError("boom");
  s.SetName("renamed");
  s.End();
  EXPECT_EQ(s.TraceParent(), "");
  EXPECT_EQ(s.TraceId(), "");
}

TEST(Tracing, ScopedParentNestsAndRestores) {
  EXPECT_EQ(CurrentParent(), "");
  {
    ScopedParent a("tp-a");
    EXPECT_EQ(CurrentParent(), "tp-a");
    {
      ScopedParent b("tp-b");
      EXPECT_EQ(CurrentParent(), "tp-b");
    }
    EXPECT_EQ(CurrentParent(), "tp-a");
  }
  EXPECT_EQ(CurrentParent(), "");
}

// Runs last in this binary: it turns tracing on (exporting to a closed port;
// export failures are dropped, never surfaced to callers).
TEST(Tracing, ZEnabledSpansPropagateTheTrace) {
  Init("tracing-test", "127.0.0.1:9");
  ASSERT_TRUE(Enabled());
  Span root = StartSpan("root", Kind::kServer);
  ASSERT_TRUE(root);
  const std::string tp = root.TraceParent();
  ASSERT_EQ(tp.size(), 55U);  // 00-<32 hex>-<16 hex>-<2 hex>
  EXPECT_EQ(tp.substr(3, 32), root.TraceId());

  Span child = StartSpan("child", Kind::kClient, tp);
  EXPECT_EQ(child.TraceId(), root.TraceId());
  EXPECT_NE(child.TraceParent(), tp);  // same trace, new span id

  {
    ScopedParent parent(tp);
    Span implicit = StartSpan("implicit", Kind::kInternal);
    EXPECT_EQ(implicit.TraceId(), root.TraceId());
  }
  Span linked = StartSpan("batch", Kind::kConsumer, "", {tp, child.TraceParent()});
  EXPECT_TRUE(linked);
  EXPECT_NE(linked.TraceId(), root.TraceId());  // links do not join the trace
  linked.End();
  child.End();
  root.End();
  Shutdown();
  EXPECT_FALSE(Enabled());
}

}  // namespace
}  // namespace cartwheel::tracing
