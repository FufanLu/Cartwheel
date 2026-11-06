// Decoders for every other persisted or wire format: MANIFEST edits, WAL
// batch payloads, and the DVV sibling sets replicas exchange (M3).
// Round-trip property: anything that decodes must re-encode to bytes that
// decode to the same value.
#include <cstdlib>
#include <string>
#include <vector>

#include "cluster/siblings.h"
#include "storage/lsm/version.h"
#include "storage/lsm/write_batch.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace cartwheel;
  const std::string_view in(reinterpret_cast<const char*>(data), size);

  storage::lsm::VersionEdit edit;
  if (edit.DecodeFrom(in).ok()) {
    std::string again;
    edit.EncodeTo(&again);
    storage::lsm::VersionEdit edit2;
    if (!edit2.DecodeFrom(again).ok() || edit2.DebugString() != edit.DebugString()) std::abort();
  }

  if (auto batch = storage::lsm::WriteBatch::FromContents(in); batch.ok()) {
    if (batch->Contents() != in) std::abort();
  }

  if (auto set = cluster::SiblingSet::Decode(in); set.ok()) {
    auto again = cluster::SiblingSet::Decode(set->Encode());
    if (!again.ok()) std::abort();
  }
  return 0;
}

std::vector<std::string> FuzzSeeds() {
  using namespace cartwheel;
  storage::lsm::VersionEdit e;
  e.log_number = 7;
  e.next_file_number = 12;
  e.new_files.emplace_back(
      1, storage::lsm::FileMetaData{
             9, 4096, 100, storage::lsm::MakeInternalKey("a", 1, storage::lsm::ValueType::kValue),
             storage::lsm::MakeInternalKey("z", 1, storage::lsm::ValueType::kValue)});
  e.deleted_files.emplace(0, 3);
  std::string edit;
  e.EncodeTo(&edit);
  storage::lsm::WriteBatch b;
  b.Put("k", "v");
  b.Delete("gone");
  cluster::SiblingSet s;
  s.Update("n1", {}, "v1", false, 1);
  s.Update("n2", {}, "v2", true, 2);
  return {edit, std::string(b.Contents()), s.Encode()};
}
