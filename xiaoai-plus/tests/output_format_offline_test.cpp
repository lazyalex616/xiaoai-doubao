#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "audio/output_format.hpp"

int main() {
  constexpr int rate = 24000;
  constexpr int samples = 2400;
  std::vector<uint8_t> f32;
  std::vector<uint8_t> s16;
  for (int i = 0; i < samples; ++i) {
    const float wave = 0.5f * std::sin(2.0 * 3.141592653589793 * 440.0 * i / rate);
    uint32_t bits;
    std::memcpy(&bits, &wave, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) f32.push_back(uint8_t(bits >> (8 * byte)));
    const auto sample = static_cast<int16_t>(std::lround(wave * 32767.0f));
    s16.push_back(uint8_t(sample));
    s16.push_back(uint8_t(uint16_t(sample) >> 8));
  }
  xiaoai_plus::audio::OutputFormat float_decoder;
  const auto float_probe = float_decoder.Inspect(f32);
  assert(float_probe.selected == xiaoai_plus::audio::OutputFormat::Kind::kF32);
  assert(float_probe.divisible_by_four && float_probe.f32_in_range_ratio == 1.0);
  assert(float_decoder.ToS16(f32) == s16);

  xiaoai_plus::audio::OutputFormat int_decoder;
  const auto int_probe = int_decoder.Inspect(s16);
  assert(int_probe.selected == xiaoai_plus::audio::OutputFormat::Kind::kS16);
  assert(int_decoder.ToS16(s16) == s16);
  int_decoder.Reset();
  assert(int_decoder.kind() == xiaoai_plus::audio::OutputFormat::Kind::kUnknown);
}
