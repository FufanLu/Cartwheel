// Whole M1 stack in one process, over real sockets:
//   HTTP client -> gateway (Beast) -> gRPC -> cart-svc -> gRPC -> wheel-node
// Runs in CI without Docker.

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <prometheus/registry.h>

#include "client/kv_client.h"
#include "common/thread_pool.h"
#include "gateway/cart_routes.h"
#include "gateway/http_server.h"
#include "kv/kv_service.h"
#include "kv/local_store.h"
#include "services/cart/cart_service.h"
#include "services/cart/cart_store.h"

namespace cartwheel {
namespace {

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;

struct HttpResult {
  int status = 0;
  nlohmann::json body;
  std::string request_id;
};

HttpResult Call(uint16_t port, http::verb method, const std::string& target,
                const std::string& body = "", const std::string& idem = "") {
  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  beast::tcp_stream stream(ioc);
  stream.connect(resolver.resolve("127.0.0.1", std::to_string(port)));
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "localhost");
  req.set(http::field::content_type, "application/json");
  if (!idem.empty()) req.set("Idempotency-Key", idem);
  req.body() = body;
  req.prepare_payload();
  http::write(stream, req);
  beast::flat_buffer buf;
  http::response<http::string_body> res;
  http::read(stream, buf, res);
  HttpResult out;
  out.status = static_cast<int>(res.result_int());
  out.request_id = std::string(res["X-Request-Id"]);
  out.body = nlohmann::json::parse(res.body(), nullptr, /*allow_exceptions=*/false);
  return out;
}

class StackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // wheel-node
    kv_service_ = std::make_unique<kv::KvServiceImpl>(&store_, registry_);
    grpc::ServerBuilder kb;
    kb.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &kv_port_);
    kb.RegisterService(kv_service_.get());
    kv_server_ = kb.BuildAndStart();
    ASSERT_GT(kv_port_, 0);

    // cart-svc
    kv_client_ = std::make_unique<client::KvClient>(
        client::KvClientOptions{.target = "127.0.0.1:" + std::to_string(kv_port_)});
    cart_store_ = std::make_unique<cart::CartStore>(kv_client_.get());
    cart_service_ =
        std::make_unique<cart::CartServiceImpl>(cart_store_.get(), &workers_, registry_);
    grpc::ServerBuilder cb;
    cb.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &cart_port_);
    cb.RegisterService(cart_service_.get());
    cart_server_ = cb.BuildAndStart();
    ASSERT_GT(cart_port_, 0);

    // gateway
    routes_ = std::make_unique<gateway::CartRoutes>(
        cart::v1::CartService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(cart_port_), grpc::InsecureChannelCredentials())),
        std::chrono::milliseconds(2000));
    routes_->Register(&router_);
    http_ = std::make_unique<gateway::HttpServer>(
        gateway::HttpServerOptions{.address = "127.0.0.1", .port = 0, .threads = 2}, &router_,
        registry_);
    ASSERT_TRUE(http_->Start().ok());
  }

  void TearDown() override {
    http_->Stop();
    cart_server_->Shutdown();
    workers_.Shutdown();
    kv_server_->Shutdown();
  }

  uint16_t port() const { return http_->port(); }

  prometheus::Registry registry_;
  kv::LocalStore store_{"wheel-1", storage::NewMemEngine()};
  std::unique_ptr<kv::KvServiceImpl> kv_service_;
  std::unique_ptr<grpc::Server> kv_server_;
  int kv_port_ = 0;
  std::unique_ptr<client::KvClient> kv_client_;
  std::unique_ptr<cart::CartStore> cart_store_;
  ThreadPool workers_{{.threads = 8, .max_queue = 256, .name = "cart-test"}};
  std::unique_ptr<cart::CartServiceImpl> cart_service_;
  std::unique_ptr<grpc::Server> cart_server_;
  int cart_port_ = 0;
  gateway::Router router_;
  std::unique_ptr<gateway::CartRoutes> routes_;
  std::unique_ptr<gateway::HttpServer> http_;
};

TEST_F(StackTest, AddGetRemoveOverHttp) {
  auto add = Call(port(), http::verb::post, "/api/v1/carts/alice/items",
                  R"({"sku":"kettle","quantity":2,"unit_price_cents":2599,"title":"Kettle"})");
  ASSERT_EQ(add.status, 200) << add.body.dump();
  EXPECT_EQ(add.body["total_cents"], 5198);
  EXPECT_FALSE(add.request_id.empty());

  auto get = Call(port(), http::verb::get, "/api/v1/carts/alice");
  ASSERT_EQ(get.status, 200);
  ASSERT_EQ(get.body["items"].size(), 1U);
  EXPECT_EQ(get.body["items"][0]["sku"], "kettle");
  EXPECT_EQ(get.body["items"][0]["quantity"], 2);

  auto del = Call(port(), http::verb::delete_, "/api/v1/carts/alice/items/kettle");
  ASSERT_EQ(del.status, 200);
  EXPECT_TRUE(del.body["items"].empty());
}

TEST_F(StackTest, IdempotencyKeyMakesRetriesSafe) {
  const std::string body = R"({"sku":"mug","quantity":1,"unit_price_cents":900})";
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(Call(port(), http::verb::post, "/api/v1/carts/bob/items", body, "click-1").status,
              200);
  }
  auto get = Call(port(), http::verb::get, "/api/v1/carts/bob");
  EXPECT_EQ(get.body["items"][0]["quantity"], 1);
}

TEST_F(StackTest, ConcurrentHttpAddsAreAllCounted) {
  constexpr int kClients = 8;
  constexpr int kEach = 10;
  std::vector<std::thread> threads;
  threads.reserve(kClients);
  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([this, c] {
      for (int i = 0; i < kEach; ++i) {
        const auto r = Call(port(), http::verb::post, "/api/v1/carts/team/items",
                            R"({"sku":"s)" + std::to_string(c) + R"(","quantity":1})");
        ASSERT_EQ(r.status, 200) << r.body.dump();
      }
    });
  }
  for (auto& t : threads) t.join();
  auto get = Call(port(), http::verb::get, "/api/v1/carts/team");
  int total = 0;
  for (const auto& it : get.body["items"]) total += it["quantity"].get<int>();
  EXPECT_EQ(total, kClients * kEach);
}

TEST_F(StackTest, ErrorsMapToHttp) {
  EXPECT_EQ(Call(port(), http::verb::get, "/nope").status, 404);
  EXPECT_EQ(Call(port(), http::verb::put, "/api/v1/carts/alice").status, 405);
  auto bad_json = Call(port(), http::verb::post, "/api/v1/carts/alice/items", "{not json");
  EXPECT_EQ(bad_json.status, 400);
  EXPECT_EQ(bad_json.body["error"]["code"], "InvalidArgument");
  auto bad_qty =
      Call(port(), http::verb::post, "/api/v1/carts/alice/items", R"({"sku":"x","quantity":500})");
  EXPECT_EQ(bad_qty.status, 400);
}

}  // namespace
}  // namespace cartwheel
