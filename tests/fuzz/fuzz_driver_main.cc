// Stand-in for libFuzzer where it is unavailable (Apple Clang ships without
// it): feeds LLVMFuzzerTestOneInput random byte strings plus byte-level
// mutations of valid seed inputs. CI builds the same targets with
// -fsanitize=fuzzer for real coverage-guided fuzzing; this driver keeps every
// target exercised in `ctest` on every platform.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
std::vector<std::string> FuzzSeeds();  // each target provides well-formed examples

int main(int argc, char** argv) {
  const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 20000;
  std::mt19937_64 rng(argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1);
  const auto seeds = FuzzSeeds();
  for (const auto& s : seeds) {
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  }
  for (long i = 0; i < iterations; ++i) {
    std::string input;
    if (!seeds.empty() && rng() % 4 != 0) {
      input = seeds[rng() % seeds.size()];
      const int flips = 1 + static_cast<int>(rng() % 4);
      for (int f = 0; f < flips && !input.empty(); ++f) {
        switch (rng() % 3) {
          case 0: input[rng() % input.size()] = static_cast<char>(rng()); break;
          case 1: input.resize(rng() % (input.size() + 1)); break;
          default: input.insert(rng() % (input.size() + 1), 1, static_cast<char>(rng())); break;
        }
      }
    } else {
      input.resize(rng() % 256);
      for (auto& c : input) c = static_cast<char>(rng());
    }
    LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(input.data()), input.size());
  }
  std::printf("%ld inputs, %zu seeds: no crashes\n", iterations, seeds.size());
  return 0;
}
