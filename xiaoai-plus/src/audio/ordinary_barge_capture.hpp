#pragma once
#include <deque>
#include <vector>
#include "audio/capture_stamp.hpp"
#include "audio/shadow_voice.hpp"

namespace xiaoai_plus::audio {
struct CapturedVoiceFrame {
  shadow::Context origin;
  uint64_t first_sample{0}, first_read_ns{0}, delivered_ns{0};
  shadow::PcmFrame pcm{};
  bool voiced{false};
};
struct BargeCaptureResult {
  bool fault{false}, confirmed{false};
  bool reference_active{false}, echo_path_confident{false};
  int echo_delay_samples{0}; // Local raw-channel relation, not ADC/DAC time.
  std::vector<CapturedVoiceFrame> frames;
};
// Single capture-thread owner. LocalInputAdapter retains its capture fault
// latch: only a real recorder restart with a larger generation can unlock it.
class OrdinaryBargeCapture {
 public:
  OrdinaryBargeCapture(shadow::PairedAec& aec, shadow::VoiceActivityDetector& vad,
                       int processing_io_delay_hint_ms);
  BargeCaptureResult Push(const shadow::Context& context, const CaptureStamp& stamp,
                          const std::vector<uint8_t>& four_channel_s16,
                          bool can_confirm, bool playback_requires_reference=false,
                          bool actual_input_needed=false);
  std::vector<CapturedVoiceFrame> TakePreroll();
  void ClearPreroll();
 private:
  shadow::PairedAec& aec_;
  shadow::VoiceActivityDetector& vad_;
  const int hint_;
  shadow::LocalInputAdapter input_;
  shadow::Context context_;
  std::deque<CapturedVoiceFrame> preroll_;
  bool ready_{false}, faulted_{false};
  uint64_t fault_generation_{0};
  size_t voice_streak_{0};
  // A bounded delay search (-2..90ms) establishes a raw-reference path before
  // near-end confirmation. A 128-tap NLMS fits the short FIR around that path.
  // Once validated, lag and weights freeze through doubletalk. A bounded
  // predictor-residual/reference check rejects coherent unmodelled echo; raw
  // mic/ref correlation falling during near speech cannot reset the path.
  // The window and predictor are capture-local; an unvalidated active path
  // remains fail-closed. This is not room/child acoustic acceptance.
  float ReferenceAt(int64_t sample) const;
  float MicrophoneAt(int64_t sample) const;
  bool FindEchoPath(int64_t end, int* delay, double* coherence) const;
  bool ResidualContainsEcho(int64_t end) const;
  std::array<float,128> echo_weights_{};
  std::array<float,4096> reference_history_{}, microphone_history_{};
  std::array<float,4096> predictor_residual_history_{};
  uint64_t history_samples_{0};
  int echo_delay_samples_{0};
  bool path_bound_{false}, fit_confident_{false};
  size_t fit_good_frames_{0}, weak_reference_frames_{0};
  size_t frozen_fit_frames_{0};
  uint64_t last_residual_check_sample_{0};
  bool residual_echo_clear_{false};
  // The echo predictor is only needed while playback/confirmation is eligible
  // and for the existing 200ms reference tail. APM/VAD still process idle PCM.
  bool echo_processing_enabled_{false};
  size_t echo_tail_frames_{0};
};
}  // namespace xiaoai_plus::audio
