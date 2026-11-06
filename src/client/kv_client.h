#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "client/kv_api.h"
#include "common/backoff.h"
#include "common/status.h"

namespace cartwheel::client {

struct KvClientOptions {
  // gRPC target, e.g. "localhost:7100" or "dns:///wheel-nodes:7100".
  // With several DNS addresses the channel round-robins across nodes.
  std::string target = "localhost:7100";
  // Per-attempt deadline. Every RPC has one: a call without a deadline can
  // hang forever and pin resources.
  int64_t timeout_ms = 500;
  // Retries for retryable codes (Unavailable, DeadlineExceeded, Throttled,
  // ResourceExhausted). Full jitter avoids synchronized retry storms.
  BackoffPolicy retry{
      .base_micros = 5'000, .cap_micros = 200'000, .max_attempts = 3, .jitter = JitterMode::kFull};
};

// Thread-safe. One client per process per cluster is the norm, because the
// underlying channel multiplexes all calls over shared HTTP/2 connections.
//
// Retrying a Put is safe but not exactly-once. A retry after a lost response
// can store the same value as an extra sibling. Applications that need
// exactly-once effects (cart AddItem) make the value itself idempotent.
class KvClient final : public KvApi {
 public:
  explicit KvClient(KvClientOptions options);
  ~KvClient() override;
  KvClient(const KvClient&) = delete;
  KvClient& operator=(const KvClient&) = delete;

  [[nodiscard]] Result<KvRecord> Get(std::string_view table, std::string_view key) override;
  [[nodiscard]] Result<KvRecord> Put(std::string_view table, std::string_view key,
                                     std::string_view value, std::string_view context) override;
  [[nodiscard]] Result<KvRecord> Delete(std::string_view table, std::string_view key,
                                        std::string_view context) override;

  // Blocks until the channel is connected or the timeout passes.
  [[nodiscard]] bool WaitForReady(int64_t timeout_ms);

 private:
  struct Impl;  // gRPC channel and stub; keeps grpcpp out of this header
  std::unique_ptr<Impl> impl_;
};

}  // namespace cartwheel::client
