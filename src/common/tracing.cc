#include "common/tracing.h"

#include <map>
#include <mutex>

#include <opentelemetry/context/context.h>
#include <opentelemetry/context/propagation/text_map_propagator.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/propagation/http_trace_context.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>

#include "common/logging.h"

namespace cartwheel::tracing {
namespace {

namespace ot = opentelemetry;
namespace trace_api = opentelemetry::trace;
namespace sdktrace = opentelemetry::sdk::trace;

struct State {
  std::mutex mu;
  std::shared_ptr<sdktrace::TracerProvider> provider;
  ot::nostd::shared_ptr<trace_api::Tracer> tracer;
};

State& state() {
  static State s;
  return s;
}

// The tracer, or null when tracing is off. Read once per span on hot paths,
// so it is cached after Init and never changes until Shutdown.
ot::nostd::shared_ptr<trace_api::Tracer>& tracer() {
  return state().tracer;
}

// A string map carrier for W3C trace context.
class MapCarrier final : public ot::context::propagation::TextMapCarrier {
 public:
  ot::nostd::string_view Get(ot::nostd::string_view key) const noexcept override {
    auto it = values.find(std::string(key));
    return it == values.end() ? ot::nostd::string_view() : ot::nostd::string_view(it->second);
  }
  void Set(ot::nostd::string_view key, ot::nostd::string_view value) noexcept override {
    values[std::string(key)] = std::string(value);
  }
  std::map<std::string, std::string> values;
};

trace_api::propagation::HttpTraceContext& propagator() {
  static trace_api::propagation::HttpTraceContext p;
  return p;
}

trace_api::SpanContext Parse(std::string_view traceparent) {
  if (traceparent.empty()) return trace_api::SpanContext::GetInvalid();
  MapCarrier c;
  c.values["traceparent"] = std::string(traceparent);
  ot::context::Context empty;
  auto ctx = propagator().Extract(c, empty);
  return trace_api::GetSpan(ctx)->GetContext();
}

trace_api::SpanKind ToKind(Kind k) {
  switch (k) {
    case Kind::kServer: return trace_api::SpanKind::kServer;
    case Kind::kClient: return trace_api::SpanKind::kClient;
    case Kind::kProducer: return trace_api::SpanKind::kProducer;
    case Kind::kConsumer: return trace_api::SpanKind::kConsumer;
    case Kind::kInternal: break;
  }
  return trace_api::SpanKind::kInternal;
}

thread_local std::string current_parent;

}  // namespace

struct Span::Impl {
  ot::nostd::shared_ptr<trace_api::Span> span;
  bool ended = false;
};

void Init(const std::string& service, const std::string& otlp_endpoint) {
  if (otlp_endpoint.empty()) return;
  ot::exporter::otlp::OtlpGrpcExporterOptions opts;
  opts.endpoint = otlp_endpoint;
  opts.use_ssl_credentials = false;
  opts.timeout = std::chrono::seconds(2);  // an export must never stall shutdown for long
  auto exporter = ot::exporter::otlp::OtlpGrpcExporterFactory::Create(opts);
  sdktrace::BatchSpanProcessorOptions bopts;
  bopts.max_queue_size = 8192;  // spans dropped beyond this; tracing must never block
  bopts.schedule_delay_millis = std::chrono::milliseconds(500);
  auto processor = sdktrace::BatchSpanProcessorFactory::Create(std::move(exporter), bopts);
  auto resource = ot::sdk::resource::Resource::Create({{"service.name", service}});
  std::shared_ptr<sdktrace::TracerProvider> provider =
      sdktrace::TracerProviderFactory::Create(std::move(processor), resource);
  std::lock_guard lock(state().mu);
  state().provider = provider;
  state().tracer = provider->GetTracer("cartwheel");
  CW_LOG_INFO("tracing: OTLP/gRPC to {} as service {}", otlp_endpoint, service);
}

void Shutdown() {
  std::lock_guard lock(state().mu);
  if (state().provider) {
    state().provider->ForceFlush();
    state().provider->Shutdown();
  }
  state().tracer = nullptr;
  state().provider.reset();
}

bool Enabled() {
  return tracer() != nullptr;
}

Span StartSpan(std::string_view name, Kind kind, std::string_view parent,
               const std::vector<std::string>& links) {
  Span out;
  auto& t = tracer();
  if (!t) return out;
  trace_api::StartSpanOptions o;
  o.kind = ToKind(kind);
  if (parent.empty()) parent = current_parent;
  if (!parent.empty()) {
    const auto pc = Parse(parent);
    if (pc.IsValid()) o.parent = pc;
  }
  out.impl_ = std::make_unique<Span::Impl>();
  if (links.empty()) {
    out.impl_->span = t->StartSpan(ot::nostd::string_view(name.data(), name.size()), o);
  } else {
    std::vector<std::pair<trace_api::SpanContext, std::map<std::string, std::string>>> ls;
    ls.reserve(links.size());
    for (const auto& l : links) {
      auto c = Parse(l);
      if (c.IsValid()) ls.emplace_back(c, std::map<std::string, std::string>{});
    }
    out.impl_->span = t->StartSpan(ot::nostd::string_view(name.data(), name.size()),
                                   std::map<std::string, std::string>{}, ls, o);
  }
  return out;
}

Span::~Span() {
  End();
}

Span::Span(Span&& other) noexcept = default;
Span& Span::operator=(Span&& other) noexcept {
  if (this != &other) {
    End();
    impl_ = std::move(other.impl_);
  }
  return *this;
}

void Span::SetAttribute(std::string_view key, std::string_view value) {
  if (impl_) {
    impl_->span->SetAttribute(ot::nostd::string_view(key.data(), key.size()),
                              ot::nostd::string_view(value.data(), value.size()));
  }
}

void Span::SetAttribute(std::string_view key, int64_t value) {
  if (impl_) impl_->span->SetAttribute(ot::nostd::string_view(key.data(), key.size()), value);
}

void Span::SetError(std::string_view message) {
  if (impl_) {
    impl_->span->SetStatus(trace_api::StatusCode::kError,
                           ot::nostd::string_view(message.data(), message.size()));
  }
}

void Span::SetName(std::string_view name) {
  if (impl_) impl_->span->UpdateName(ot::nostd::string_view(name.data(), name.size()));
}

void Span::End() {
  if (impl_ && !impl_->ended) {
    impl_->ended = true;
    impl_->span->End();
  }
}

std::string Span::TraceParent() const {
  if (!impl_) return {};
  MapCarrier c;
  ot::context::Context empty;
  auto ctx = trace_api::SetSpan(empty, impl_->span);
  propagator().Inject(c, ctx);
  return c.values["traceparent"];
}

std::string Span::TraceId() const {
  if (!impl_) return {};
  char buf[32];
  impl_->span->GetContext().trace_id().ToLowerBase16(buf);
  return {buf, sizeof(buf)};
}

const std::string& CurrentParent() {
  return current_parent;
}

ScopedParent::ScopedParent(std::string traceparent)
    : previous_(std::exchange(current_parent, std::move(traceparent))) {}

ScopedParent::~ScopedParent() {
  current_parent = std::move(previous_);
}

}  // namespace cartwheel::tracing
