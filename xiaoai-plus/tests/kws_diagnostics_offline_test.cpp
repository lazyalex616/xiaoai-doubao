#include "audio/kws_diagnostics.hpp"
#include "audio/kws_preprocess.hpp"
#include "wakeup/trigger.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

int main() {
  // Normal capture conversion feeds both KWS and the cloud uplink.  The
  // recorded 24-bit samples would saturate with the former shift=8 default.
  const int32_t captured[] = {0x10000000, -0x10000000, 0x7fff0000, -0x7fff0000};
  int16_t uplink[4]{};
  xiaoai_plus::audio::ConvertKwsS32ToS16(captured, 4, 16, uplink);
  assert(uplink[0] == 4096 && uplink[1] == -4096);
  assert(uplink[2] == 32767 && uplink[3] == -32767);

  using xiaoai_plus::audio::KwsDiagnostics;
  using xiaoai_plus::wakeup::Trigger;

  KwsDiagnostics stats(4);
  // Two interleaved frames: three microphone channels and one playback reference.
  const int16_t capture[] = {1000, -1000, 32767, 0, -1000, 1000, -32768, 0};
  stats.ObserveCaptureS16(capture, 2);
  const int16_t kws[] = {2000, -2000};
  stats.ObserveKwsMonoS16(kws, 2);
  stats.RecordDuration(KwsDiagnostics::Stage::kS32ToS16, 500000);
  stats.RecordDuration(KwsDiagnostics::Stage::kS32ToS16, 3000000);
  stats.FeedRecorderStderr("arecord: over");
  stats.FeedRecorderStderr("run!!! (at least 0.064 ms long)\nother warning\n");
  stats.FeedRecorderStderr("overrun without newline");
  stats.FeedRecorderStderr("");

  const auto first = stats.TakeSnapshotAndReset();
  assert(first.capture_channels.size() == 4);
  assert(first.capture_channels[0].samples == 2);
  assert(std::abs(first.capture_channels[0].rms - 1000.0) < 0.001);
  assert(first.capture_channels[2].peak == 32768);
  assert(first.capture_channels[2].clipped == 2);
  assert(first.capture_channels[3].rms == 0);
  assert(first.kws_mono.peak == 2000);
  assert(first.overrun_lines == 2);
  const auto conversion = first.durations[static_cast<size_t>(KwsDiagnostics::Stage::kS32ToS16)];
  assert(conversion.calls == 2);
  assert(std::abs(conversion.mean_ms - 1.75) < 0.001);
  assert(conversion.max_ms == 3.0);
  assert(conversion.p95_upper_ms == 5.0);
  const auto resample = first.durations[static_cast<size_t>(KwsDiagnostics::Stage::kResample)];
  assert(resample.calls == 0);  // Current 16 kHz capture -> 16 kHz KWS path.
  assert(stats.TakeSnapshotAndReset().capture_channels[0].samples == 0);

  std::vector<std::string> accepted;
  Trigger trigger({"豆包", "豆包豆包"}, [&](const std::string& label) {
    accepted.push_back(label);
    return true;
  });
  assert(trigger.FireFromText("豆包"));
  assert(trigger.FireFromText("豆包豆包"));
  assert(!trigger.FireFromText("请叫豆包"));
  assert(!trigger.FireFromText("豆包同学"));
  assert(accepted.size() == 2);
  assert(accepted[0] == "豆包");
  assert(accepted[1] == "豆包豆包");
}
