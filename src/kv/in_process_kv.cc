#include "kv/in_process_kv.h"

namespace cartwheel::kv {
namespace {

template <typename Resp>
client::KvRecord ToRecord(const Resp& resp, uint32_t replicas) {
  client::KvRecord rec;
  for (const auto& v : resp.versions()) rec.values.push_back(v.value());
  rec.context = resp.context();
  rec.replicas = replicas;
  return rec;
}

}  // namespace

InProcessKv::InProcessKv(LocalStore* store) : service_(store, registry_) {}

Result<client::KvRecord> InProcessKv::Get(std::string_view table, std::string_view key) {
  v1::GetRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  v1::GetResponse resp;
  CW_RETURN_IF_ERROR(service_.HandleGet(req, &resp));
  return ToRecord(resp, resp.replicas_responded());
}

Result<client::KvRecord> InProcessKv::Put(std::string_view table, std::string_view key,
                                          std::string_view value, std::string_view context) {
  v1::PutRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  req.set_value(std::string(value));
  req.set_context(std::string(context));
  v1::PutResponse resp;
  CW_RETURN_IF_ERROR(service_.HandlePut(req, &resp));
  return ToRecord(resp, resp.replicas_acked());
}

Result<client::KvRecord> InProcessKv::Delete(std::string_view table, std::string_view key,
                                             std::string_view context) {
  v1::DeleteRequest req;
  req.set_table(std::string(table));
  req.set_key(std::string(key));
  req.set_context(std::string(context));
  v1::DeleteResponse resp;
  CW_RETURN_IF_ERROR(service_.HandleDelete(req, &resp));
  return ToRecord(resp, resp.replicas_acked());
}

}  // namespace cartwheel::kv
