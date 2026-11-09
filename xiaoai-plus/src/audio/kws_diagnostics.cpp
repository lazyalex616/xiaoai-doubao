#include "audio/kws_diagnostics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace xiaoai_plus::audio {

namespace {
constexpr std::array<uint64_t, 7> kBucketUpperNs = {
    1000000, 2000000, 5000000, 10000000, 20000000, 50000000, 100000000};

bool ContainsOverrun(const std::string& line) {
  constexpr char needle[] = "overrun";
  return std::search(line.begin(), line.end(), std::begin(needle),
                     std::end(needle) - 1,
                     [](unsigned char a, unsigned char b) {
                       if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
                       return a == b;
                     }) != line.end();
}
}  // namespace

KwsDiagnostics::KwsDiagnostics(size_t capture_channels)
    : capture_(capture_channels), raw_capture_(capture_channels) {
  if (capture_channels == 0 || capture_channels > 16) {
    throw std::invalid_argument("capture_channels must be 1..16");
  }
}

void KwsDiagnostics::ObserveCaptureS32(const int32_t* interleaved, size_t frames) {
  if (!interleaved || frames == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t f = 0; f < frames; ++f) {
    for (size_t ch = 0; ch < raw_capture_.size(); ++ch) {
      const int32_t sample = interleaved[f * raw_capture_.size() + ch];
      auto& dst = raw_capture_[ch];
      const uint32_t magnitude = sample == std::numeric_limits<int32_t>::min()
                                     ? 2147483648u
                                     : static_cast<uint32_t>(std::abs(static_cast<int64_t>(sample)));
      ++dst.samples;
      dst.peak_abs = std::max(dst.peak_abs, magnitude);
      if (sample < -8388608 || sample > 8388607) ++dst.outside_signed_24;
      if ((static_cast<uint32_t>(sample) & 0xffu) == 0) ++dst.low_byte_zero;
      if (sample == std::numeric_limits<int32_t>::min() ||
          sample == std::numeric_limits<int32_t>::max()) ++dst.full_scale_32;
    }
  }
}

void KwsDiagnostics::Observe(LevelAccum* dst, int16_t sample) {
  const int32_t magnitude = sample == std::numeric_limits<int16_t>::min()
                                ? 32768 : std::abs(static_cast<int32_t>(sample));
  ++dst->samples;
  dst->sum_squares += static_cast<long double>(sample) * sample;
  dst->peak = std::max(dst->peak, static_cast<uint16_t>(magnitude));
  if (sample == std::numeric_limits<int16_t>::min() ||
      sample == std::numeric_limits<int16_t>::max()) {
    ++dst->clipped;
  }
}

void KwsDiagnostics::ObserveCaptureS16(const int16_t* interleaved, size_t frames) {
  if (!interleaved || frames == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t f = 0; f < frames; ++f) {
    for (size_t ch = 0; ch < capture_.size(); ++ch) {
      Observe(&capture_[ch], interleaved[f * capture_.size() + ch]);
    }
  }
}

void KwsDiagnostics::ObserveKwsMonoS16(const int16_t* mono, size_t samples) {
  if (!mono || samples == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (size_t i = 0; i < samples; ++i) Observe(&kws_mono_, mono[i]);
}

void KwsDiagnostics::RecordDuration(Stage stage, uint64_t nanoseconds) {
  const auto index = static_cast<size_t>(stage);
  if (index >= durations_.size()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto& dst = durations_[index];
  ++dst.calls;
  dst.total_ns += nanoseconds;
  dst.max_ns = std::max(dst.max_ns, nanoseconds);
  size_t bucket = 0;
  while (bucket < kBucketUpperNs.size() && nanoseconds > kBucketUpperNs[bucket]) ++bucket;
  ++dst.buckets[bucket];
}

void KwsDiagnostics::FeedRecorderStderr(std::string_view text) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (text.empty()) {
    if (ContainsOverrun(stderr_line_)) ++overrun_lines_;
    stderr_line_.clear();
    return;
  }
  for (char c : text) {
    if (c == '\n' || c == '\r') {
      if (ContainsOverrun(stderr_line_)) ++overrun_lines_;
      stderr_line_.clear();
    } else if (stderr_line_.size() < 256) {
      stderr_line_.push_back(c);
    }
  }
}

void KwsDiagnostics::RecordOverrun() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++overrun_lines_;
}

KwsDiagnostics::Level KwsDiagnostics::Summarize(const LevelAccum& src) {
  Level out;
  out.samples = src.samples;
  out.rms = src.samples ? std::sqrt(src.sum_squares / src.samples) : 0.0;
  out.peak = src.peak;
  out.clipped = src.clipped;
  return out;
}

KwsDiagnostics::Duration KwsDiagnostics::Summarize(const DurationAccum& src) {
  Duration out;
  out.calls = src.calls;
  if (!src.calls) return out;
  out.mean_ms = static_cast<double>(src.total_ns / src.calls / 1000000.0L);
  out.max_ms = static_cast<double>(src.max_ns) / 1000000.0;
  const uint64_t target = (src.calls * 95 + 99) / 100;
  uint64_t seen = 0;
  for (size_t i = 0; i < src.buckets.size(); ++i) {
    seen += src.buckets[i];
    if (seen >= target) {
      out.p95_upper_ms = i < kBucketUpperNs.size()
                             ? static_cast<double>(kBucketUpperNs[i]) / 1000000.0
                             : std::numeric_limits<double>::infinity();
      break;
    }
  }
  return out;
}

KwsDiagnostics::Snapshot KwsDiagnostics::TakeSnapshotAndReset() {
  std::lock_guard<std::mutex> lock(mutex_);
  Snapshot out;
  out.raw_capture_channels.reserve(raw_capture_.size());
  for (auto& channel : raw_capture_) {
    out.raw_capture_channels.push_back(RawLevel{channel.samples, channel.peak_abs,
        channel.outside_signed_24, channel.low_byte_zero, channel.full_scale_32});
    channel = {};
  }
  out.capture_channels.reserve(capture_.size());
  for (auto& channel : capture_) {
    out.capture_channels.push_back(Summarize(channel));
    channel = {};
  }
  out.kws_mono = Summarize(kws_mono_);
  kws_mono_ = {};
  for (size_t i = 0; i < durations_.size(); ++i) {
    out.durations[i] = Summarize(durations_[i]);
    durations_[i] = {};
  }
  out.overrun_lines = overrun_lines_;
  overrun_lines_ = 0;
  return out;
}

}  // namespace xiaoai_plus::audio
