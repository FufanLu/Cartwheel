#pragma once

#include "client/kv_api.h"
#include "kv/kv_service.h"
#include "kv/local_store.h"

namespace cartwheel::kv {

// KvApi backed directly by a LocalStore via the same handlers the gRPC service
// uses: identical validation, DVV semantics and context encoding, no network.
class InProcessKv final : public client::KvApi {
 public:
  explicit InProcessKv(LocalStore* store);

  Result<client::KvRecord> Get(std::string_view table, std::string_view key) override;
  Result<client::KvRecord> Put(std::string_view table, std::string_view key, std::string_view value,
                               std::string_view context) override;
  Result<client::KvRecord> Delete(std::string_view table, std::string_view key,
                                  std::string_view context) override;

 private:
  prometheus::Registry registry_;  // private: tests should not see these metrics
  KvServiceImpl service_;
};

}  // namespace cartwheel::kv
