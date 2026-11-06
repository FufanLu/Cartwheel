// SSTable data/index blocks: decoding, iteration and seeks on arbitrary bytes.
#include <memory>
#include <string>
#include <vector>

#include "storage/lsm/block.h"
#include "storage/lsm/dbformat.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace cartwheel::storage::lsm;
  auto block =
      std::make_shared<const Block>(std::string(reinterpret_cast<const char*>(data), size));
  auto it = Block::NewIterator(block);
  size_t n = 0;
  for (it->SeekToFirst(); it->Valid() && n < 100000; it->Next()) ++n;
  it->Seek(MakeInternalKey("m", kMaxSequence, kValueTypeForSeek));
  if (it->Valid()) it->Next();
  return 0;
}

std::vector<std::string> FuzzSeeds() {
  using namespace cartwheel::storage::lsm;
  BlockBuilder b(4);
  for (int i = 0; i < 40; ++i) {
    b.Add(MakeInternalKey("key" + std::to_string(100 + i), 5, ValueType::kValue), "val");
  }
  return {std::string(b.Finish())};
}
