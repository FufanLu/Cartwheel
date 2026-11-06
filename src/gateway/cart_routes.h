#pragma once

#include <chrono>
#include <memory>

#include <nlohmann/json_fwd.hpp>

#include "cartwheel/cart/v1/cart.grpc.pb.h"
#include "gateway/http.h"

namespace cartwheel::gateway {

// REST facade for cart-svc:
//   GET    /api/v1/carts/{user}
//   POST   /api/v1/carts/{user}/items          body: {sku, quantity, unit_price_cents, title}
//                                              header: Idempotency-Key (optional)
//   DELETE /api/v1/carts/{user}/items/{sku}
//   DELETE /api/v1/carts/{user}
class CartRoutes {
 public:
  CartRoutes(std::shared_ptr<cart::v1::CartService::Stub> stub,
             std::chrono::milliseconds upstream_timeout);
  void Register(Router* router);

  [[nodiscard]] static nlohmann::json CartToJson(const cart::v1::Cart& cart);

 private:
  boost::asio::awaitable<Response> Get(RequestContext& ctx);
  boost::asio::awaitable<Response> AddItem(RequestContext& ctx);
  boost::asio::awaitable<Response> RemoveItem(RequestContext& ctx);
  boost::asio::awaitable<Response> Clear(RequestContext& ctx);

  std::shared_ptr<cart::v1::CartService::Stub> stub_;
  std::chrono::milliseconds timeout_;
};

}  // namespace cartwheel::gateway
