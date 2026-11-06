#include <gtest/gtest.h>

#include "gateway/http.h"

namespace cartwheel::gateway {
namespace {

boost::asio::awaitable<Response> Noop(RequestContext& /*ctx*/) {
  co_return Response{};
}

TEST(SplitPathTest, DecodesAndIgnoresQuery) {
  std::vector<std::string> parts;
  ASSERT_TRUE(SplitPath("/api/v1/carts/u%201/items?x=1", &parts));
  EXPECT_EQ(parts, (std::vector<std::string>{"api", "v1", "carts", "u 1", "items"}));
  ASSERT_TRUE(SplitPath("//double//slash/", &parts));
  EXPECT_EQ(parts, (std::vector<std::string>{"double", "slash"}));
  EXPECT_FALSE(SplitPath("/bad%zz", &parts));
  EXPECT_FALSE(SplitPath("/trunc%2", &parts));
}

TEST(RouterTest, MatchesParamsAndMethods) {
  Router r;
  r.Add(http::verb::get, "/api/v1/carts/{user}", Noop);
  r.Add(http::verb::delete_, "/api/v1/carts/{user}/items/{sku}", Noop);

  auto m = r.Find(http::verb::delete_, "/api/v1/carts/alice/items/sku%2F9");
  ASSERT_NE(m.handler, nullptr);
  EXPECT_EQ(m.params.at("user"), "alice");
  EXPECT_EQ(m.params.at("sku"), "sku/9");
  EXPECT_EQ(m.route, "/api/v1/carts/{user}/items/{sku}");

  auto wrong_method = r.Find(http::verb::post, "/api/v1/carts/alice");
  EXPECT_EQ(wrong_method.handler, nullptr);
  EXPECT_TRUE(wrong_method.path_known);

  auto unknown = r.Find(http::verb::get, "/nope");
  EXPECT_EQ(unknown.handler, nullptr);
  EXPECT_FALSE(unknown.path_known);
}

TEST(HttpStatusTest, MapsStatusCodes) {
  EXPECT_EQ(HttpStatusFor(StatusCode::kNotFound), http::status::not_found);
  EXPECT_EQ(HttpStatusFor(StatusCode::kThrottled), http::status::too_many_requests);
  EXPECT_EQ(HttpStatusFor(StatusCode::kResourceExhausted), http::status::service_unavailable);
  EXPECT_EQ(HttpStatusFor(StatusCode::kDeadlineExceeded), http::status::gateway_timeout);
  EXPECT_EQ(HttpStatusFor(StatusCode::kCorruption), http::status::internal_server_error);
}

}  // namespace
}  // namespace cartwheel::gateway
