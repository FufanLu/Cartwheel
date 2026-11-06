#include "gateway/http.h"

#include <stdexcept>

#include <nlohmann/json.hpp>

namespace cartwheel::gateway {
namespace {

int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool PercentDecode(std::string_view in, std::string* out) {
  out->clear();
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] != '%') {
      out->push_back(in[i]);
      continue;
    }
    if (i + 2 >= in.size()) return false;
    const int hi = HexValue(in[i + 1]);
    const int lo = HexValue(in[i + 2]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<char>(hi * 16 + lo));
    i += 2;
  }
  return true;
}

}  // namespace

bool SplitPath(std::string_view target, std::vector<std::string>* out) {
  out->clear();
  if (const auto q = target.find('?'); q != std::string_view::npos) target = target.substr(0, q);
  size_t pos = 0;
  while (pos < target.size()) {
    if (target[pos] == '/') {
      ++pos;
      continue;
    }
    const size_t end = std::min(target.find('/', pos), target.size());
    std::string seg;
    if (!PercentDecode(target.substr(pos, end - pos), &seg)) return false;
    out->push_back(std::move(seg));
    pos = end;
  }
  return true;
}

void Router::Add(http::verb method, std::string pattern, Handler handler) {
  std::vector<std::string> segments;
  if (!SplitPath(pattern, &segments)) throw std::invalid_argument("bad route pattern: " + pattern);
  routes_.push_back({method, std::move(pattern), std::move(segments), std::move(handler)});
}

Router::Match Router::Find(http::verb method, std::string_view target) const {
  Match match;
  std::vector<std::string> parts;
  if (!SplitPath(target, &parts)) return match;
  for (const auto& route : routes_) {
    if (route.segments.size() != parts.size()) continue;
    PathParams params;
    bool ok = true;
    for (size_t i = 0; i < parts.size() && ok; ++i) {
      const auto& seg = route.segments[i];
      if (seg.size() > 2 && seg.front() == '{' && seg.back() == '}') {
        if (parts[i].empty()) ok = false;
        params.emplace(seg.substr(1, seg.size() - 2), parts[i]);
      } else {
        ok = seg == parts[i];
      }
    }
    if (!ok) continue;
    match.path_known = true;
    if (route.method != method) continue;
    match.handler = &route.handler;
    match.params = std::move(params);
    match.route = route.pattern;
    return match;
  }
  return match;
}

http::status HttpStatusFor(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::kOk: return http::status::ok;
    case StatusCode::kNotFound: return http::status::not_found;
    case StatusCode::kInvalidArgument: return http::status::bad_request;
    case StatusCode::kAlreadyExists:
    case StatusCode::kFailedPrecondition:
    case StatusCode::kAborted: return http::status::conflict;
    case StatusCode::kThrottled: return http::status::too_many_requests;
    case StatusCode::kResourceExhausted:
    case StatusCode::kUnavailable: return http::status::service_unavailable;
    case StatusCode::kDeadlineExceeded: return http::status::gateway_timeout;
    case StatusCode::kUnimplemented: return http::status::not_implemented;
    default: return http::status::internal_server_error;
  }
}

Response JsonResponse(http::status status, const nlohmann::json& body, const RequestContext& ctx) {
  Response res{status, ctx.request.version()};
  res.set(http::field::content_type, "application/json");
  res.set(http::field::cache_control, "no-store");
  res.set("X-Request-Id", ctx.request_id);
  res.keep_alive(ctx.request.keep_alive());
  res.body() = body.dump();
  res.prepare_payload();
  return res;
}

Response ErrorResponse(const Status& s, const RequestContext& ctx) {
  Response res =
      JsonResponse(HttpStatusFor(s.code()),
                   {{"error", {{"code", StatusCodeName(s.code())}, {"message", s.message()}}},
                    {"request_id", ctx.request_id}},
                   ctx);
  if (s.retryable()) res.set(http::field::retry_after, "1");
  return res;
}

}  // namespace cartwheel::gateway
