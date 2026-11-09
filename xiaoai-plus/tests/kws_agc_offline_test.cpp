#include <cassert>
#include <algorithm>
#include <vector>
#include "audio/kws_preprocess.hpp"
using xiaoai_plus::audio::KwsAgc;
int main() {
#ifdef NDEBUG
#error AGC tests require active assertions
#endif
  KwsAgc silent;
  std::vector<int16_t> zero(16000, 0);
  silent.Process(zero.data(), zero.size());
  assert(std::all_of(zero.begin(), zero.end(), [](int16_t x) { return x == 0; }));
  assert(silent.gain() >= 1.0 && silent.gain() <= 15.85);
  std::vector<int16_t> floor(16000, 1);
  silent.Process(floor.data(), floor.size());
  assert(*std::max_element(floor.begin(), floor.end()) <= 16);
  KwsAgc loud;
  std::vector<int16_t> strong(16000, 30000);
  loud.Process(strong.data(), strong.size());
  assert(loud.gain() == 1.0);
  assert(std::all_of(strong.begin(), strong.end(), [](int16_t x) { return x == 30000; }));
  // Silence followed by a transient: bounded saturation, no integer wrap.
  std::vector<int16_t> transient(160, 30000);
  silent.Process(transient.data(), transient.size());
  assert(std::all_of(transient.begin(), transient.end(), [](int16_t x) { return x > 0; }));
  assert(silent.gain() >= 1.0 && silent.gain() <= 15.85);
  std::vector<int16_t> input(14400);
  for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<int16_t>((i * 7919) % 2001 - 1000);
  auto split = input;
  KwsAgc a, b;
  a.Process(input.data(), input.size());
  for (size_t i = 0; i < split.size(); i += 1440) b.Process(split.data() + i, 1440);
  assert(input == split && a.gain() == b.gain());
  assert(a.gain() >= 1.0 && a.gain() <= 15.85);
}
