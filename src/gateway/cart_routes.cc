#include "gateway/cart_routes.h"

#include <grpcpp/client_context.h>
#include <nlohmann/json.hpp>

#include "gateway/grpc_await.h"
#include "kv/grpc_status.h"

namespace cartwheel::gateway {
namespace {

using cart::v1::Cart;

void PrepareContext(grpc::ClientContext* g, const RequestContext& ctx,
                    std::chrono::milliseconds timeout) {
  g->set_deadline(std::chrono::system_clock::now() + timeout);
  g->AddMetadata("x-request-id", ctx.request_id);
}

Response CartOrError(const grpc::Status& s, const Cart& cart, const RequestContext& ctx) {
  if (!s.ok()) return ErrorResponse(kv::FromGrpc(s), ctx);
  return JsonResponse(http::status::ok, CartRoutes::CartToJson(cart), ctx);
}

}  // namespace

CartRoutes::CartRoutes(std::shared_ptr<cart::v1::CartService::Stub> stub,
                       std::chrono::milliseconds upstream_timeout, const Authenticator* auth,
                       bool allow_anonymous)
    : stub_(std::move(stub)),
      timeout_(upstream_timeout),
      auth_(auth),
      allow_anonymous_(allow_anonymous) {}

void CartRoutes::Register(Router* router) {
  using Method = boost::asio::awaitable<Response> (CartRoutes::*)(RequestContext&);
  auto guarded = [this](Method method) -> Handler {
    return [this, method](RequestContext& c) -> boost::asio::awaitable<Response> {
      if (auth_ != nullptr) {
        if (auto s = auth_->AuthorizeUser(c.request, c.params.at("user"), allow_anonymous_);
            !s.ok()) {
          co_return ErrorResponse(s, c);
        }
      }
      co_return co_await (this->*method)(c);
    };
  };
  router->Add(http::verb::get, "/api/v1/carts/{user}", guarded(&CartRoutes::Get));
  router->Add(http::verb::post, "/api/v1/carts/{user}/items", guarded(&CartRoutes::AddItem));
  router->Add(http::verb::delete_, "/api/v1/carts/{user}/items/{sku}",
              guarded(&CartRoutes::RemoveItem));
  router->Add(http::verb::delete_, "/api/v1/carts/{user}", guarded(&CartRoutes::Clear));
}

nlohmann::json CartRoutes::CartToJson(const Cart& cart) {
  nlohmann::json items = nlohmann::json::array();
  for (const auto& it : cart.items()) {
    items.push_back({{"sku", it.sku()},
                     {"quantity", it.quantity()},
                     {"unit_price_cents", it.unit_price_cents()},
                     {"title", it.title()}});
  }
  return {{"user_id", cart.user_id()},
          {"items", std::move(items)},
          {"total_cents", cart.total_cents()},
          {"merged_siblings", cart.merged_siblings()}};
}

boost::asio::awaitable<Response> CartRoutes::Get(RequestContext& ctx) {
  grpc::ClientContext g;
  PrepareContext(&g, ctx, timeout_);
  cart::v1::GetCartRequest req;
  req.set_user_id(ctx.params.at("user"));
  Cart resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->GetCart(&g, &req, &resp, std::move(done)); });
  co_return CartOrError(s, resp, ctx);
}

boost::asio::awaitable<Response> CartRoutes::AddItem(RequestContext& ctx) {
  cart::v1::AddItemRequest req;
  req.set_user_id(ctx.params.at("user"));
  try {
    const auto body = nlohmann::json::parse(ctx.request.body());
    auto* item = req.mutable_item();
    item->set_sku(body.at("sku").get<std::string>());
    item->set_quantity(body.value("quantity", 1U));
    item->set_unit_price_cents(body.value("unit_price_cents", int64_t{0}));
    item->set_title(body.value("title", std::string()));
  } catch (const nlohmann::json::exception& e) {
    co_return ErrorResponse(Status::InvalidArgument(std::string("bad JSON body: ") + e.what()),
                            ctx);
  }
  req.set_idempotency_key(std::string(ctx.request["Idempotency-Key"].substr(0, 128)));

  grpc::ClientContext g;
  PrepareContext(&g, ctx, timeout_);
  Cart resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->AddItem(&g, &req, &resp, std::move(done)); });
  co_return CartOrError(s, resp, ctx);
}

boost::asio::awaitable<Response> CartRoutes::RemoveItem(RequestContext& ctx) {
  grpc::ClientContext g;
  PrepareContext(&g, ctx, timeout_);
  cart::v1::RemoveItemRequest req;
  req.set_user_id(ctx.params.at("user"));
  req.set_sku(ctx.params.at("sku"));
  Cart resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->RemoveItem(&g, &req, &resp, std::move(done)); });
  co_return CartOrError(s, resp, ctx);
}

boost::asio::awaitable<Response> CartRoutes::Clear(RequestContext& ctx) {
  grpc::ClientContext g;
  PrepareContext(&g, ctx, timeout_);
  cart::v1::ClearCartRequest req;
  req.set_user_id(ctx.params.at("user"));
  Cart resp;
  const grpc::Status s = co_await AwaitGrpc(
      [&](auto done) { stub_->async()->ClearCart(&g, &req, &resp, std::move(done)); });
  co_return CartOrError(s, resp, ctx);
}

}  // namespace cartwheel::gateway
