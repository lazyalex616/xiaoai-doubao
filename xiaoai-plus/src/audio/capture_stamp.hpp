#pragma once
#include <cstdint>

namespace xiaoai_plus::audio {
// Issued by the actual recorder read worker. These are local pipe acquisition
// timestamps, never ADC timestamps or a measured acoustic delay.
struct CaptureStamp {
  uint64_t generation{0}, first_sample{0};
  uint64_t first_read_ns{0}, delivered_ns{0};
  bool continuous{false};
};
}  // namespace xiaoai_plus::audio
