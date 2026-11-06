// Whole-table open + lookups + full scan on arbitrary bytes (footer, index,
// bloom filter and data blocks all attacker-controlled).
#include <memory>
#include <string>
#include <vector>

#include "mem_files.h"
#include "storage/lsm/table.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace cartwheel;
  using namespace cartwheel::storage::lsm;
  auto file = std::make_unique<fuzz::MemRandomAccessFile>(
      std::string(reinterpret_cast<const char*>(data), size));
  auto table = Table::Open(std::move(file), 1, nullptr);
  if (!table.ok()) return 0;
  std::string v;
  static_cast<void>((*table)->Get(LookupKey("key105", kMaxSequence), &v));
  auto it = Table::NewIterator(*table);
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid() && n < 100000; it->Next()) ++n;
  return 0;
}

std::vector<std::string> FuzzSeeds() {
  using namespace cartwheel;
  using namespace cartwheel::storage::lsm;
  class Sink final : public storage::WritableFile {
   public:
    std::string data;
    Status Append(std::string_view d) override {
      data.append(d);
      return {};
    }
    Status Flush() override { return {}; }
    Status Sync() override { return {}; }
    Status Close() override { return {}; }
    uint64_t Size() const override { return data.size(); }
  };
  Sink sink;
  TableBuilder tb(TableOptions{.block_size = 256}, &sink);
  for (int i = 0; i < 60; ++i) {
    tb.Add(MakeInternalKey("key" + std::to_string(100 + i), 9, ValueType::kValue), "value");
  }
  static_cast<void>(tb.Finish());
  return {sink.data};
}
