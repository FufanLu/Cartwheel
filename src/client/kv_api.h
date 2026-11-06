#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/status.h"

namespace cartwheel::client {

// What a read or write returns. More than one value means concurrent siblings,
// and the application must reconcile them (cart-svc merges them as an OR-Set).
struct KvRecord {
  std::vector<std::string> values;
  std::string context;  // opaque; pass to the next Put/Delete
  uint32_t replicas = 0;

  [[nodiscard]] bool found() const noexcept { return !values.empty(); }
};

// The Dynamo-style API applications program against. KvClient implements it
// over gRPC; kv::InProcessKv implements it over a LocalStore so services can
// be tested against the real DVV logic without a network.
class KvApi {
 public:
  virtual ~KvApi() = default;
  [[nodiscard]] virtual Result<KvRecord> Get(std::string_view table, std::string_view key) = 0;
  [[nodiscard]] virtual Result<KvRecord> Put(std::string_view table, std::string_view key,
                                             std::string_view value, std::string_view context) = 0;
  [[nodiscard]] virtual Result<KvRecord> Delete(std::string_view table, std::string_view key,
                                                std::string_view context) = 0;
};

}  // namespace cartwheel::client
