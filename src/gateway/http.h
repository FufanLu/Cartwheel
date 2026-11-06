#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json_fwd.hpp>

#include "common/status.h"

namespace cartwheel::gateway {

namespace http = boost::beast::http;
using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;
using PathParams = std::unordered_map<std::string, std::string>;

// Per-request context handed to handlers.
struct RequestContext {
  const Request& request;
  PathParams params;
  std::string request_id;
  std::string route;  // matched pattern, used as a low-cardinality metric label
};

using Handler = std::function<boost::asio::awaitable<Response>(RequestContext&)>;

// Method + path-pattern router. Patterns use {name} segments:
//   "/api/v1/carts/{user}/items/{sku}"
class Router {
 public:
  void Add(http::verb method, std::string pattern, Handler handler);

  struct Match {
    const Handler* handler = nullptr;
    PathParams params;
    std::string route;
    bool path_known = false;  // path matched but method did not -> 405
  };
  [[nodiscard]] Match Find(http::verb method, std::string_view target) const;

 private:
  struct Route {
    http::verb method;
    std::string pattern;
    std::vector<std::string> segments;
    Handler handler;
  };
  std::vector<Route> routes_;
};

// Splits "/a/b%20c?x=1" into {"a", "b c"}. Returns false on a malformed escape.
[[nodiscard]] bool SplitPath(std::string_view target, std::vector<std::string>* out);

// Response helpers. All JSON responses carry X-Request-Id.
[[nodiscard]] Response JsonResponse(http::status status, const nlohmann::json& body,
                                    const RequestContext& ctx);
[[nodiscard]] Response ErrorResponse(const Status& s, const RequestContext& ctx);
[[nodiscard]] http::status HttpStatusFor(StatusCode code) noexcept;

}  // namespace cartwheel::gateway
