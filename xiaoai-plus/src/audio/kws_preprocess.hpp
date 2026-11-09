#pragma once

#include <cstddef>
#include <cstdint>

namespace xiaoai_plus::audio {

// Convert S32 capture to S16 without retaining the input. Shift 8 tests a
// right-justified 24-bit hypothesis; shift 16 maps the full S32 range.
void ConvertKwsS32ToS16(const int32_t* input, size_t samples, int shift,
                        int16_t* output);

// Frame-causal RMS AGC for the KWS copy only. Input must contain complete
// 10 ms frames at 16 kHz (capture uses 1440 samples = nine frames). A final
// short frame is allowed for offline EOF replay; state persists across calls.
class KwsAgc {
 public:
  void Process(int16_t* samples, size_t count);
  double gain() const { return gain_; }
 private:
  double envelope_{0.0};
  double gain_{1.0};
};

}  // namespace xiaoai_plus::audio
