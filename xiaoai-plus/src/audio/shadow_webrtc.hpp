#pragma once

#include "audio/shadow_voice.hpp"

#include <memory>

namespace xiaoai_plus::audio::shadow {

// Local adapter status codes are separate from public APM return codes.
constexpr int kApmNotCalled = -10000;
constexpr int kUnsupportedShadowFormat = -10001;
constexpr int kInvalidPair = -10002;
constexpr int kNonFiniteResidual = -10003;

struct ApmFormat { int sample_rate_hz{16000}; size_t channels{1}; };
struct ApmInitReceipt {
  int format_status{kApmNotCalled};
  bool created{false};
  int enable_status{kApmNotCalled}, tuning_status{kApmNotCalled};
  int initialize_status{kApmNotCalled};
  int likelihood_status{kApmNotCalled};
  int vad_frame_size_ms{0}, vad_likelihood{-1};
  bool only_requested_component{false}, ready{false};
};
struct AecStepReceipt {
  int pair_status{kApmNotCalled};
  AecHealth health;
  // The inherited optional argument is an APM processing I/O delay hint.
  // It is not an ADC timestamp or room acoustic propagation measurement.
  std::optional<int> processing_io_delay_hint_ms;
  bool residual_valid{false};
  int capture_api_status{kApmNotCalled};
  size_t nonfinite_residual_samples{0};
};

// Local, serialized caller only. No APM allocation occurs until Reset().
// Every failed step invalidates the instance until an explicit Reset().
// Reset is a new-stream operation; the shadow owner enforces capture epochs.
class PairedWebrtcAec final : public PairedAec {
 public:
  // The default public ExtendedFilter(true) matches existing AEC tuning.
  // Disabling it exists for the fixed-library failure regression only.
  explicit PairedWebrtcAec(ApmFormat format = {}, bool extended_filter = true);
  ~PairedWebrtcAec() override;
  BackendKind kind() const override { return BackendKind::kRealBackend; }
  bool Reset() override;
  AecResult Process(const PairedFrame& frame,
                    std::optional<int> processing_io_delay_hint_ms) override;
  const ApmInitReceipt& initialization() const;
  const AecStepReceipt& last_step() const;
  size_t successful_pairs() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// A separate APM with only its public VoiceDetection component enabled.
// A positive result means APM voice detection, without speaker/child identity.
class WebrtcVoiceDetector final : public VoiceActivityDetector {
 public:
  explicit WebrtcVoiceDetector(ApmFormat format = {});
  ~WebrtcVoiceDetector() override;
  BackendKind kind() const override { return BackendKind::kRealBackend; }
  bool Reset() override;
  VadResult Analyze(const PcmFrame& cleaned) override;
  const ApmInitReceipt& initialization() const;
  const VadResult& last_result() const;
  size_t successful_frames() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xiaoai_plus::audio::shadow
