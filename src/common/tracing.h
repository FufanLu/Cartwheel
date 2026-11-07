#pragma once

// Distributed tracing on OpenTelemetry (OTLP/gRPC to a collector or Jaeger).
//
// Context crosses process boundaries as a W3C `traceparent` string: the
// HTTP header at the edge, gRPC metadata between services, a Kafka record
// header between deal-svc and order-worker. Inside a process, a span is
// passed explicitly (async code) or set as the thread's current parent with
// ScopedParent (synchronous worker-pool code), so clients such as KvClient
// can pick it up without changing every signature.
//
// With no endpoint configured, Init is a no-op and every Span is a cheap
// null object, so instrumented code costs almost nothing when tracing is off.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cartwheel::tracing {

enum class Kind : uint8_t { kInternal, kServer, kClient, kProducer, kConsumer };

// otlp_endpoint like "localhost:4317"; empty disables tracing.
void Init(const std::string& service, const std::string& otlp_endpoint);
// Flushes buffered spans. Call before exit.
void Shutdown();
[[nodiscard]] bool Enabled();

class Span {
 public:
  Span() = default;
  ~Span();
  Span(Span&& other) noexcept;
  Span& operator=(Span&& other) noexcept;
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

  void SetAttribute(std::string_view key, std::string_view value);
  void SetAttribute(std::string_view key, int64_t value);
  void SetError(std::string_view message);
  void SetName(std::string_view name);
  void End();

  // W3C traceparent of this span ("" for a null span).
  [[nodiscard]] std::string TraceParent() const;
  // 32 hex digits ("" for a null span).
  [[nodiscard]] std::string TraceId() const;
  [[nodiscard]] explicit operator bool() const { return impl_ != nullptr; }

  struct Impl;

 private:
  friend Span StartSpan(std::string_view, Kind, std::string_view, const std::vector<std::string>&);
  std::unique_ptr<Impl> impl_;
};

// A root or remote-child span. `parent` is a traceparent (may be empty).
// `links` are traceparents of related spans (for example the records in a
// batch), shown as references.
Span StartSpan(std::string_view name, Kind kind, std::string_view parent = {},
               const std::vector<std::string>& links = {});

// The calling thread's current parent (set by ScopedParent), or "".
[[nodiscard]] const std::string& CurrentParent();

// Makes `traceparent` the thread's current parent for its lifetime.
class ScopedParent {
 public:
  explicit ScopedParent(std::string traceparent);
  ~ScopedParent();
  ScopedParent(const ScopedParent&) = delete;
  ScopedParent& operator=(const ScopedParent&) = delete;

 private:
  std::string previous_;
};

}  // namespace cartwheel::tracing
