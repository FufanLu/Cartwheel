#pragma once

#include <chrono>
#include <memory>

#include <nlohmann/json_fwd.hpp>

#include "cartwheel/cart/v1/cart.grpc.pb.h"
#include "gateway/auth.h"
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
  // With `auth`, a request carrying a Bearer token may only touch the token's
  // own cart; requests without one are allowed only if `allow_anonymous`.
  CartRoutes(std::shared_ptr<cart::v1::CartService::Stub> stub,
             std::chrono::milliseconds upstream_timeout, const Authenticator* auth = nullptr,
             bool allow_anonymous = true);
  void Register(Router* router);

  [[nodiscard]] static nlohmann::json CartToJson(const cart::v1::Cart& cart);

 private:
  boost::asio::awaitable<Response> Get(RequestContext& ctx);
  boost::asio::awaitable<Response> AddItem(RequestContext& ctx);
  boost::asio::awaitable<Response> RemoveItem(RequestContext& ctx);
  boost::asio::awaitable<Response> Clear(RequestContext& ctx);

  std::shared_ptr<cart::v1::CartService::Stub> stub_;
  std::chrono::milliseconds timeout_;
  const Authenticator* auth_;
  bool allow_anonymous_;
};

}  // namespace cartwheel::gateway
