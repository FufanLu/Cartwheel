#include "common/status.h"

#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace cartwheel {
namespace {

TEST(StatusTest, DefaultIsOk) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), StatusCode::kOk);
  EXPECT_EQ(s.ToString(), "OK");
}

TEST(StatusTest, CarriesCodeAndMessage) {
  const Status s = Status::NotFound("key 'cart:42'");
  EXPECT_FALSE(s.ok());
  EXPECT_TRUE(s.IsNotFound());
  EXPECT_EQ(s.ToString(), "NotFound: key 'cart:42'");
}

TEST(StatusTest, WithContextKeepsCode) {
  const Status s = Status::IOError("disk full").WithContext("append wal");
  EXPECT_EQ(s.code(), StatusCode::kIOError);
  EXPECT_EQ(s.message(), "append wal: disk full");
  EXPECT_TRUE(Status::Ok().WithContext("ignored").ok());
}

TEST(StatusTest, RetryabilityFollowsCode) {
  EXPECT_TRUE(Status::Unavailable("x").retryable());
  EXPECT_TRUE(Status::Throttled("x").retryable());
  EXPECT_FALSE(Status::Corruption("x").retryable());
  EXPECT_FALSE(Status::InvalidArgument("x").retryable());
}

Result<int> ParsePositive(int v) {
  if (v <= 0) return Status::InvalidArgument("must be positive");
  return v;
}

Result<int> Doubled(int v) {
  CW_ASSIGN_OR_RETURN(const int x, ParsePositive(v));
  return x * 2;
}

Status Check(int v) {
  CW_RETURN_IF_ERROR(ParsePositive(v).status());
  return Status::Ok();
}

TEST(ResultTest, ValueAndErrorPaths) {
  auto ok = Doubled(21);
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ(*ok, 42);

  auto bad = Doubled(-1);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.status().code(), StatusCode::kInvalidArgument);
  EXPECT_EQ(bad.value_or(7), 7);

  EXPECT_TRUE(Check(1).ok());
  EXPECT_EQ(Check(0).code(), StatusCode::kInvalidArgument);
}

TEST(ResultTest, MoveOnlyValue) {
  Result<std::unique_ptr<std::string>> r(std::make_unique<std::string>("cart"));
  ASSERT_TRUE(r.ok());
  std::unique_ptr<std::string> owned = std::move(r).value();
  EXPECT_EQ(*owned, "cart");
}

}  // namespace
}  // namespace cartwheel
