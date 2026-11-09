#include "audio/kws_preprocess.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace xiaoai_plus::audio {

void ConvertKwsS32ToS16(const int32_t* input, size_t samples, int shift,
                        int16_t* output) {
  if (shift != 8 && shift != 16) {
    throw std::invalid_argument("KWS S32 shift must be 8 or 16");
  }
  if (!input || !output) return;
  for (size_t i = 0; i < samples; ++i) {
    const int32_t scaled = input[i] >> shift;
    output[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, scaled)));
  }
}

void KwsAgc::Process(int16_t* samples, size_t count) {
  if (!samples) return;
  for (size_t offset = 0; offset < count; offset += 160) {
    const size_t n = std::min<size_t>(160, count - offset);
    double energy = 0.0;
    for (size_t j = 0; j < n; ++j) {
      const double x = samples[offset + j] / 32768.0;
      energy += x * x;
    }
    envelope_ = 0.9 * envelope_ + 0.1 * energy / n;
    const double desired = std::clamp(0.0562 / std::max(std::sqrt(envelope_), 1e-6),
                                      1.0, 15.85);
    gain_ = 0.9 * gain_ + 0.1 * desired;
    for (size_t j = 0; j < n; ++j) {
      const double amplified = samples[offset + j] * gain_;
      // Saturate before conversion: loud transients cannot wrap around S16.
      samples[offset + j] = static_cast<int16_t>(
          std::clamp(std::nearbyint(amplified), -32768.0, 32767.0));
    }
  }
}

}  // namespace xiaoai_plus::audio
