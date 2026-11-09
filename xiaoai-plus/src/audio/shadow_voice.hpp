#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace xiaoai_plus::audio::shadow {

constexpr size_t kSamplesPerFrame = 160;  // mono PCM16, 16 kHz, 10 ms
constexpr size_t kCacheFrames = 20;       // provisional 200 ms / 6400 PCM bytes
constexpr uint64_t kWindowNs = 200000000;
using PcmFrame = std::array<int16_t, kSamplesPerFrame>;

struct Context {
  uint64_t epoch{0}, session{0}, response{0}, playback{0}, ingress{0}, capture{0};
  bool valid() const;
  bool operator==(const Context& other) const;
  bool operator!=(const Context& other) const { return !(*this == other); }
};

struct FrameStamp {
  Context context;
  uint64_t mic_sample{0}, reference_sample{0};
  // Monotonic LOCAL DELIVERY time. Neither ADC time nor acoustic delay.
  uint64_t delivered_ns{0};
};
struct PairedFrame { FrameStamp stamp; PcmFrame mic{}, reference{}; };

enum class InputFault {
  kNone, kUnbound, kContext, kMissingReference, kMalformed, kOverflow,
  kSequence, kDeliveryGap, kCaptureFault
};
// Local/offline input ONLY. No Recorder, App, Player or sender connection.
// One source, interleaved little-endian 4ch PCM16: ch0 mic, ch3 reference.
struct LocalChunk {
  Context context;
  uint64_t mic_sample{0}, reference_sample{0}, delivered_ns{0};
  const uint8_t* pcm{nullptr};
  size_t bytes{0};
  bool reference_present{false};
};
struct PairedBatch {
  InputFault fault{InputFault::kNone};
  std::array<PairedFrame, 9> frames{};  // at most one 1440-sample delivery
  size_t count{0};
};
// Serialized by its local caller; keeps less than one frame of partial PCM.
class LocalInputAdapter {
 public:
  // capture is an owner-issued monotonic physical acquisition generation.
  // Same-capture rebinds preserve sample/delivery watermarks. After any fault,
  // only a genuinely restarted acquisition with larger capture can unlock.
  bool Begin(const Context& context);
  // Quarantines this capture; does not erase consumed/discarded watermarks.
  void ClearContinuity();
  PairedBatch Push(const LocalChunk& chunk);
  size_t pending_samples() const { return pending_; }
 private:
  Context context_{};
  PairedFrame partial_{};
  size_t pending_{0};
  bool has_delivery_{false};
  bool faulted_{false};
  uint64_t capture_frontier_{0};
  uint64_t next_sample_{0}, last_delivery_ns_{0};
};

enum class BackendKind { kUnavailable, kInjectedFixture, kRealBackend };
// Every status starts fail-closed. A PCM payload is not proof of AEC success.
struct AecHealth {
  bool initialized{false}, reverse_processed{false}, capture_processed{false};
  int reverse_status{-1}, capture_status{-1};
  bool delay_applied{false};
  int delay_status{-1};
  bool healthy() const;
};
struct AecResult { AecHealth health; PcmFrame cleaned{}; };
class PairedAec {
 public:
  virtual ~PairedAec() = default;
  virtual BackendKind kind() const = 0;
  virtual bool Reset() = 0;
  // Implementations must process reverse THEN capture for this exact pair.
  // No delay value means UNKNOWN, not zero. No raw-PCM fallback is valid.
  virtual AecResult Process(const PairedFrame& frame,
                            std::optional<int> acoustic_delay_ms) = 0;
};
enum class Voice { kUnknown, kNonVoice, kVoice };
struct VadResult {
  bool initialized{false};
  int process_status{-1};
  Voice voice{Voice::kUnknown};
};
// A real implementation must use a documented VAD dependency, not EchoStats,
// raw level/correlation, private WebRtcVad ABI declarations or a fake model.
class VoiceActivityDetector {
 public:
  virtual ~VoiceActivityDetector() = default;
  virtual BackendKind kind() const = 0;
  virtual bool Reset() = 0;
  virtual VadResult Analyze(const PcmFrame& cleaned) = 0;
};

struct Candidate {
  Context context;
  uint64_t start_sample{0}, reference_start_sample{0}, start_delivery_ns{0};
  BackendKind aec_kind{BackendKind::kUnavailable};
  BackendKind vad_kind{BackendKind::kUnavailable};
};
struct ShadowResult {
  InputFault input_fault{InputFault::kNone};
  size_t paired_frames{0}, aec_rejected{0}, vad_rejected{0}, discarded_frames{0};
  size_t candidate_events{0};
  std::optional<Candidate> last_candidate;
};

// No speech-confirmation, cancellation or PCM-export API. Candidates are
// observations only. Default off; this file is absent from production CMake.
class ShadowVoiceCandidate {
 public:
  ShadowVoiceCandidate(PairedAec& aec, VoiceActivityDetector& vad,
                       bool enabled = false);
  // False on invalid/stale/fault-latched capture or unavailable DSP. Repeating
  // Begin with the same context does not reinitialize or rewind this capture.
  bool Begin(const Context& context);
  ShadowResult Push(const LocalChunk& chunk);
  void Expire(uint64_t local_now_ns);
  bool enabled() const { return enabled_; }
  size_t cached_frames() const { return cached_; }
  size_t cached_pcm_bytes() const { return cached_ * sizeof(PcmFrame); }
  const std::optional<Candidate>& candidate() const { return candidate_; }
 private:
  void ClearWindow();
  void LatchFault();
  bool Process(const PairedFrame& frame, ShadowResult& result);
  PairedAec& aec_;
  VoiceActivityDetector& vad_;
  const bool enabled_;
  bool aec_ready_{false}, vad_ready_{false};
  Context context_{};
  LocalInputAdapter input_;
  std::array<PcmFrame, kCacheFrames> pcm_cache_{};
  std::array<FrameStamp, kCacheFrames> stamps_{};
  size_t cached_{0}, write_{0}, voiced_frames_{0};
  std::optional<Candidate> streak_, candidate_;
};

}  // namespace xiaoai_plus::audio::shadow
