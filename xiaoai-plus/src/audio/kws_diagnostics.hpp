#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace xiaoai_plus::audio {

// Aggregates numbers only. PCM is inspected in place and is never copied,
// retained, written to disk, or included in diagnostics output.
class KwsDiagnostics {
 public:
  enum class Stage : size_t { kS32ToS16, kDownmix, kResample, kKws, kCount };

  struct Level {
    uint64_t samples{0};
    double rms{0.0};            // S16 sample units, 32768 = full scale
    uint16_t peak{0};           // absolute S16 peak, including |-32768|
    uint64_t clipped{0};        // samples equal to either S16 rail
  };

  struct RawLevel {
    uint64_t samples{0};
    uint32_t peak_abs{0};
    uint64_t outside_signed_24{0};  // Not representable in signed 24 bits.
    uint64_t low_byte_zero{0};
    uint64_t full_scale_32{0};
  };

  struct Duration {
    uint64_t calls{0};
    double mean_ms{0.0};
    double max_ms{0.0};
    double p95_upper_ms{0.0};  // histogram bucket upper bound, not exact p95
  };

  struct Snapshot {
    std::vector<RawLevel> raw_capture_channels;
    std::vector<Level> capture_channels;
    Level kws_mono;
    std::array<Duration, static_cast<size_t>(Stage::kCount)> durations;
    uint64_t overrun_lines{0};
  };

  explicit KwsDiagnostics(size_t capture_channels);

  void ObserveCaptureS16(const int16_t* interleaved, size_t frames);
  void ObserveCaptureS32(const int32_t* interleaved, size_t frames);
  void ObserveKwsMonoS16(const int16_t* mono, size_t samples);
  void RecordDuration(Stage stage, uint64_t nanoseconds);
  // Feed *only* arecord stderr. A line is counted once if it contains
  // "overrun"; the line itself is discarded and never returned or logged.
  void FeedRecorderStderr(std::string_view text);
  void RecordOverrun();
  Snapshot TakeSnapshotAndReset();

 private:
  struct LevelAccum {
    uint64_t samples{0};
    long double sum_squares{0.0};
    uint16_t peak{0};
    uint64_t clipped{0};
  };
  struct RawLevelAccum {
    uint64_t samples{0};
    uint32_t peak_abs{0};
    uint64_t outside_signed_24{0};
    uint64_t low_byte_zero{0};
    uint64_t full_scale_32{0};
  };
  struct DurationAccum {
    uint64_t calls{0};
    long double total_ns{0.0};
    uint64_t max_ns{0};
    std::array<uint64_t, 8> buckets{};
  };

  static void Observe(LevelAccum* dst, int16_t sample);
  static Level Summarize(const LevelAccum& src);
  static Duration Summarize(const DurationAccum& src);

  std::mutex mutex_;
  std::vector<LevelAccum> capture_;
  std::vector<RawLevelAccum> raw_capture_;
  LevelAccum kws_mono_;
  std::array<DurationAccum, static_cast<size_t>(Stage::kCount)> durations_{};
  uint64_t overrun_lines_{0};
  std::string stderr_line_;
};

}  // namespace xiaoai_plus::audio
