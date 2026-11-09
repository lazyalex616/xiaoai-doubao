#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace xiaoai_plus::audio {

// Seeduplex 3.0 documents "pcm" but does not specify its sample width.
// Decide from non-silent decoded samples before handing bytes to S16_LE aplay.
class OutputFormat {
 public:
  enum class Kind { kUnknown, kS16, kF32 };
  struct Probe {
    size_t bytes{0};
    bool divisible_by_four{false};
    double s16_rms{0};
    double s16_clipped_ratio{0};
    double f32_in_range_ratio{0};
    double f32_rms{0};
    Kind selected{Kind::kUnknown};
  };

  Probe Inspect(const std::vector<uint8_t>& raw) {
    Probe probe;
    probe.bytes = raw.size();
    probe.divisible_by_four = raw.size() % 4 == 0;
    double s16_sum = 0;
    size_t clipped = 0;
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
      const auto sample = static_cast<int16_t>(uint16_t(raw[i]) | (uint16_t(raw[i + 1]) << 8));
      const double normalized = double(sample) / 32768.0;
      s16_sum += normalized * normalized;
      if (sample <= -32760 || sample >= 32760) ++clipped;
    }
    if (raw.size() >= 2) {
      probe.s16_rms = std::sqrt(s16_sum / (raw.size() / 2));
      probe.s16_clipped_ratio = double(clipped) / (raw.size() / 2);
    }
    size_t in_range = 0;
    size_t meaningful = 0;
    double f32_sum = 0;
    for (size_t i = 0; i + 3 < raw.size(); i += 4) {
      const float sample = FloatAt(raw, i);
      if (std::isfinite(sample) && sample >= -1.0f && sample <= 1.0f) {
        ++in_range;
        f32_sum += double(sample) * sample;
        if (std::fabs(sample) >= 0.0001f) ++meaningful;
      }
    }
    const size_t count = raw.size() / 4;
    if (count) {
      probe.f32_in_range_ratio = double(in_range) / count;
      probe.f32_rms = std::sqrt(f32_sum / count);
    }
    if (kind_ == Kind::kUnknown && raw.size() >= 64) {
      if (probe.divisible_by_four && probe.f32_in_range_ratio >= 0.98 &&
          meaningful >= count / 10 && probe.f32_rms >= 0.0001) {
        kind_ = Kind::kF32;
      } else if (!probe.divisible_by_four ||
                 (probe.f32_in_range_ratio < 0.95 && probe.s16_rms >= 0.001)) {
        kind_ = Kind::kS16;
      }
    }
    probe.selected = kind_;
    return probe;
  }

  std::vector<uint8_t> ToS16(const std::vector<uint8_t>& raw) const {
    if (kind_ != Kind::kF32) return raw;
    if (raw.size() % 4 != 0) throw std::runtime_error("f32 output is not sample aligned");
    std::vector<uint8_t> out;
    out.reserve(raw.size() / 2);
    for (size_t i = 0; i < raw.size(); i += 4) {
      const float sample = FloatAt(raw, i);
      if (!std::isfinite(sample)) throw std::runtime_error("non-finite f32 output sample");
      const double clamped = std::max(-1.0, std::min(1.0, double(sample)));
      const int32_t scaled = static_cast<int32_t>(std::lround(clamped * 32767.0));
      const auto value = static_cast<uint16_t>(static_cast<int16_t>(scaled));
      out.push_back(static_cast<uint8_t>(value));
      out.push_back(static_cast<uint8_t>(value >> 8));
    }
    return out;
  }

  Kind kind() const { return kind_; }
  void Reset() { kind_ = Kind::kUnknown; }

 private:
  static float FloatAt(const std::vector<uint8_t>& raw, size_t i) {
    const uint32_t bits = uint32_t(raw[i]) | (uint32_t(raw[i + 1]) << 8) |
                          (uint32_t(raw[i + 2]) << 16) | (uint32_t(raw[i + 3]) << 24);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  Kind kind_{Kind::kUnknown};
};

}  // namespace xiaoai_plus::audio
