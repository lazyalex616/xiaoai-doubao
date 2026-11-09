#include "audio/shadow_webrtc.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xiaoai_plus::audio::shadow;
namespace {
void Require(bool okay, const char* what) { if (!okay) throw std::runtime_error(what); }
const Context kContext{1, 2, 3, 4, 5, 6};
PairedFrame Frame(uint64_t sample, const Context& context = kContext) {
  PairedFrame frame;
  frame.stamp = {context, sample, sample, 1000000000 + sample * 62500};
  return frame;
}
bool Zero(const PcmFrame& samples) {
  for (auto sample : samples) if (sample) return false;
  return true;
}
void Init(const char* label, const ApmInitReceipt& init) {
  std::cout << label << " format=" << init.format_status << " create=" << init.created
            << " enable=" << init.enable_status << " tune=" << init.tuning_status
            << " initialize=" << init.initialize_status << " isolated="
            << init.only_requested_component << " ready=" << init.ready
            << " likelihood_rc=" << init.likelihood_status << " vad_frame_ms="
            << init.vad_frame_size_ms << " vad_likelihood=" << init.vad_likelihood << '\n';
}
void Steps(const char* label, const PairedWebrtcAec& aec, const VadResult* vad = nullptr) {
  const auto& s = aec.last_step(); const auto& h = s.health;
  std::cout << label << " pair=" << s.pair_status << " initialized=" << h.initialized
            << " reverse_called=" << h.reverse_processed << " reverse=" << h.reverse_status
            << " delay_called=" << h.delay_applied << " delay=" << h.delay_status
            << " capture_called=" << h.capture_processed << " capture=" << h.capture_status
            << " capture_api_rc=" << s.capture_api_status
            << " healthy=" << h.healthy() << " residual=" << s.residual_valid
            << " nonfinite=" << s.nonfinite_residual_samples;
  if (vad) std::cout << " vad_init=" << vad->initialized << " vad_rc="
                    << vad->process_status << " voice=" << int(vad->voice);
  std::cout << '\n';
}
void WritePcm(std::ofstream& stream, const PcmFrame& pcm) {
  for (auto sample : pcm) {
    const uint16_t value = static_cast<uint16_t>(sample);
    const char bytes[] = {static_cast<char>(value & 255), static_cast<char>(value >> 8)};
    stream.write(bytes, 2);
  }
  Require(bool(stream), "PCM evidence write failed");
}
std::vector<uint8_t> SilenceChunk(size_t samples) { return std::vector<uint8_t>(samples * 8); }
LocalChunk Chunk(std::vector<uint8_t>& pcm, uint64_t start, uint64_t delivery,
                 bool reference = true, Context context = kContext) {
  return {context, start, start, delivery, pcm.data(), pcm.size(), reference};
}

void FailureBoundaries() {
  PairedWebrtcAec aec; WebrtcVoiceDetector vad;
  Require(!aec.Process(Frame(0), 0).health.initialized, "AEC processed before Reset");
  Require(vad.Analyze({}).voice == Voice::kUnknown, "VAD before Reset was not unknown");
  Require(aec.Reset() && vad.Reset(), "real APM initialization failed");
  Init("aec-init", aec.initialization()); Init("vad-init", vad.initialization());
  auto missing = aec.Process(Frame(0), std::nullopt);
  Steps("UNKNOWN_IO_DELAY", aec);
  Require(missing.health.reverse_processed && missing.health.reverse_status == 0 &&
          missing.health.capture_processed && missing.health.capture_status == -11 &&
          !missing.health.delay_applied && !missing.health.healthy() && Zero(missing.cleaned),
          "missing delay did not preserve real APM -11 and fail closed");
  Require(!aec.last_step().residual_valid && !aec.Process(Frame(160), 0).health.initialized,
          "failed instance continued before Reset");
  for (int bad : {-1, 501}) {
    Require(aec.Reset(), "AEC reset after unknown failed");
    auto result = aec.Process(Frame(0), bad); Steps("INVALID_IO_DELAY_HINT", aec);
    Require(result.health.reverse_status == 0 && result.health.delay_applied &&
            result.health.delay_status == -13 && !result.health.capture_processed &&
            !result.health.healthy() && Zero(result.cleaned), "delay warning was hidden");
  }
  Require(aec.Reset(), "AEC reset failed");
  Require(aec.Process(Frame(0), 0).health.healthy(), "synthetic known-zero failed");
  auto unknown_again = aec.Process(Frame(160), std::nullopt);
  Steps("KNOWN_HINT_THEN_UNKNOWN", aec);
  Require(unknown_again.health.capture_status == -11 && Zero(unknown_again.cleaned),
          "previous known hint was reused for current unknown delay");
  for (int fault = 0; fault < 5; ++fault) {
    Require(aec.Reset(), "pair reset failed");
    Require(aec.Process(Frame(0), 0).health.healthy(), "pair anchor failed");
    auto frame = Frame(160);
    if (fault == 0) frame.stamp.reference_sample += 1;
    if (fault == 1) frame.stamp.mic_sample = frame.stamp.reference_sample = 0;
    if (fault == 2) frame.stamp.context.capture += 1;
    if (fault == 3) frame.stamp.delivered_ns = 1;
    if (fault == 4) frame.stamp.mic_sample = frame.stamp.reference_sample =
        std::numeric_limits<uint64_t>::max();
    auto rejected = aec.Process(frame, 0); Steps("INVALID_PAIR", aec);
    Require(aec.last_step().pair_status == kInvalidPair && !rejected.health.reverse_processed &&
            !rejected.health.capture_processed && Zero(rejected.cleaned), "bad pair reached APM");
  }
  for (ApmFormat format : {ApmFormat{8000, 1}, ApmFormat{16000, 2}, ApmFormat{0, 0}}) {
    PairedWebrtcAec bad_aec(format); WebrtcVoiceDetector bad_vad(format);
    Require(!bad_aec.Reset() && !bad_vad.Reset(), "invalid format initialized");
    Init("bad-aec-init", bad_aec.initialization()); Init("bad-vad-init", bad_vad.initialization());
    Require(!bad_aec.initialization().created && !bad_vad.initialization().created &&
            bad_aec.initialization().format_status == kUnsupportedShadowFormat &&
            bad_vad.Analyze({}).voice == Voice::kUnknown, "bad format leaked output");
  }
  for (int reset = 0; reset < 40; ++reset) {
    Require(aec.Reset() && vad.Reset(), "repeated backend reset failed");
    const auto result = aec.Process(Frame(0), 0);
    const auto voice = vad.Analyze(result.cleaned);
    Require(result.health.healthy() && voice.initialized && voice.process_status == 0 &&
            voice.voice == Voice::kNonVoice && aec.successful_pairs() == 1 &&
            vad.successful_frames() == 1, "reset retained previous stream state");
  }
  std::cout << "PASS failure_boundaries_and_40_new_stream_resets\n";
}

void Synthetic(const std::string& directory) {
  std::ofstream mic(directory + "/synthetic-mic.pcm16le", std::ios::binary);
  std::ofstream reference(directory + "/synthetic-reference.pcm16le", std::ios::binary);
  std::ofstream residual(directory + "/synthetic-residual.pcm16le", std::ios::binary);
  Require(bool(mic) && bool(reference) && bool(residual), "cannot open synthetic evidence");
  PairedWebrtcAec aec; WebrtcVoiceDetector vad;
  Require(aec.Reset() && vad.Reset(), "synthetic initialization failed");
  uint32_t noise = 1234567; size_t changed_samples = 0, voice_frames = 0;
  for (size_t n = 0; n < 180; ++n) {
    auto frame = Frame(n * kSamplesPerFrame);
    for (size_t i = 0; i < kSamplesPerFrame; ++i) {
      noise = noise * 1664525 + 1013904223;
      const int far = int((noise >> 16) & 8191) - 4096;
      const double phase = double(n * kSamplesPerFrame + i) / 16000.0;
      // Co-indexed synthetic mixture with configured zero PROCESSING I/O hint.
      // This is not a speech recording, child voice, ADC alignment or a room test.
      frame.reference[i] = n < 20 ? 0 : static_cast<int16_t>(far);
      frame.mic[i] = n < 20 ? 0 : static_cast<int16_t>(far / 2 +
          (n >= 90 ? 2500 * std::sin(2 * 3.141592653589793 * 220 * phase) : 0));
    }
    const auto cleaned = aec.Process(frame, 0);
    if (!cleaned.health.healthy()) Steps(("SYNTHETIC_FAILED_FRAME_" + std::to_string(n)).c_str(), aec);
    Require(cleaned.health.healthy() && aec.last_step().residual_valid,
            "real synthetic reverse/delay/capture failed");
    const auto voice = vad.Analyze(cleaned.cleaned);
    Require(voice.initialized && voice.process_status == 0 && voice.voice != Voice::kUnknown,
            "real residual VAD failed");
    for (size_t i = 0; i < kSamplesPerFrame; ++i)
      if (cleaned.cleaned[i] != frame.mic[i]) ++changed_samples;
    if (voice.voice == Voice::kVoice) ++voice_frames;
    WritePcm(mic, frame.mic); WritePcm(reference, frame.reference); WritePcm(residual, cleaned.cleaned);
    Steps(("SYNTHETIC_FRAME_" + std::to_string(n)).c_str(), aec, &voice);
  }
  Require(aec.successful_pairs() == 180 && vad.successful_frames() == 180,
          "synthetic paired execution count mismatch");
  std::cout << "PASS synthetic_api_only frames=180 changed_samples=" << changed_samples
            << " actual_voice_flags=" << voice_frames
            << " NO_SPEECH_RATE_OR_ECHO_ACCEPTANCE_ASSERTION\n";
}

void RealNonfiniteBoundary() {
  PairedWebrtcAec aec({}, false); WebrtcVoiceDetector vad;
  Require(aec.Reset() && vad.Reset(), "non-extended APM init failed");
  uint32_t noise = 1234567; bool rejected = false;
  for (size_t n = 0; n < 180; ++n) {
    auto frame = Frame(n * kSamplesPerFrame);
    for (size_t i = 0; i < kSamplesPerFrame; ++i) {
      noise = noise * 1664525 + 1013904223;
      const int far = int((noise >> 16) & 8191) - 4096;
      const double phase = double(n * kSamplesPerFrame + i) / 16000.0;
      frame.reference[i] = n < 20 ? 0 : static_cast<int16_t>(far);
      frame.mic[i] = n < 20 ? 0 : static_cast<int16_t>(far / 2 +
          (n >= 90 ? 2500 * std::sin(2 * 3.141592653589793 * 220 * phase) : 0));
    }
    auto result = aec.Process(frame, 0);
    if (!result.health.healthy()) {
      Steps(("NON_EXTENDED_REAL_FAILURE_FRAME_" + std::to_string(n)).c_str(), aec);
      Require(aec.last_step().capture_api_status == 0 &&
              aec.last_step().nonfinite_residual_samples > 0 &&
              result.health.capture_status == kNonFiniteResidual && Zero(result.cleaned) &&
              !aec.last_step().residual_valid && vad.successful_frames() == n,
              "actual nonfinite APM output was not isolated");
      Require(!aec.Process(Frame((n + 1) * kSamplesPerFrame), 0).health.initialized,
              "nonfinite failure instance continued");
      rejected = true; break;
    }
    Require(vad.Analyze(result.cleaned).process_status == 0, "pre-fault real VAD failed");
  }
  Require(rejected, "fixed ARM APM non-extended nonfinite regression no longer reproduced");
  std::cout << "PASS actual_APM_API0_nonfinite_runtime_rejection NO_LIBRARY_ROOT_CAUSE_CLAIM\n";
}

// Counts calls while preserving the real adapter's configuration/API results.
struct CountAec final : PairedAec {
  explicit CountAec(ApmFormat format) : real(format) {}
  PairedWebrtcAec real;
  size_t resets{0}, processes{0};
  BackendKind kind() const override { return real.kind(); }
  bool Reset() override { ++resets; return real.Reset(); }
  AecResult Process(const PairedFrame& frame, std::optional<int> delay) override {
    ++processes; return real.Process(frame, delay);
  }
};
struct CountVad final : VoiceActivityDetector {
  explicit CountVad(ApmFormat format) : real(format) {}
  WebrtcVoiceDetector real;
  size_t resets{0}, processes{0};
  BackendKind kind() const override { return real.kind(); }
  bool Reset() override { ++resets; return real.Reset(); }
  VadResult Analyze(const PcmFrame& frame) override { ++processes; return real.Analyze(frame); }
};
void FailedInitializationLatch() {
  for (bool fail_aec : {false, true}) {
    const ApmFormat good{16000, 1}, bad{8000, 1};
    CountAec aec(fail_aec ? bad : good); CountVad vad(fail_aec ? good : bad);
    ShadowVoiceCandidate shadow(aec, vad, true);
    Require(!shadow.Begin(kContext), "bad real format did not fail initial Begin");
    const auto aec_resets = aec.resets, vad_resets = vad.resets;
    auto same_capture = kContext; ++same_capture.session; ++same_capture.epoch;
    Require(!shadow.Begin(same_capture) && aec.resets == aec_resets && vad.resets == vad_resets,
            "failed initialization retried real backends in same capture");
    auto bytes = SilenceChunk(1440);
    // Failed Begin must retain the previously bound context. Test its latched
    // fault separately from the rejected replacement context's context error.
    const auto result = shadow.Push(Chunk(bytes, 1440, 1000000000, true, kContext));
    Require(result.input_fault == InputFault::kCaptureFault && !result.paired_frames &&
            aec.processes == 0 && vad.processes == 0 && !shadow.cached_frames() &&
            !result.last_candidate, "failed initialization accepted new same-capture PCM");
    const auto rebound = shadow.Push(Chunk(bytes, 1440, 1000000000, true, same_capture));
    Require(rebound.input_fault != InputFault::kNone && !rebound.paired_frames &&
            aec.processes == 0 && vad.processes == 0 && !rebound.last_candidate,
            "rejected replacement context reached real backend");
    std::cout << "INIT_FAILURE_LATCH fail_aec=" << fail_aec << " bound_fault="
              << int(result.input_fault) << " rejected_rebind_fault=" << int(rebound.input_fault)
              << " AEC_Reset=" << aec.resets << " VAD_Reset=" << vad.resets << '\n';
    // A simulated new acquisition token tests lifecycle only, not device capture.
    ++same_capture.capture;
    Require(!shadow.Begin(same_capture) && aec.resets == aec_resets + 1 &&
            vad.resets == vad_resets + (fail_aec ? 0 : 1),
            "new capture did not attempt fail-fast actual Reset once");
  }
  std::cout << "PASS real_adapter_bad_format_init_latch_no_same_capture_retry\n";
}

void BatchesAndShadow() {
  LocalInputAdapter input; input.Begin(kContext);
  auto bytes = SilenceChunk(1440);
  auto batch = input.Push(Chunk(bytes, 0, 1000000000));
  Require(batch.fault == InputFault::kNone && batch.count == 9 && input.pending_samples() == 0,
          "90ms delivery failed to split 9 pairs");
  PairedWebrtcAec aec; WebrtcVoiceDetector vad;
  Require(aec.Reset() && vad.Reset(), "batch real backend reset failed");
  for (size_t n = 0; n < batch.count; ++n) {
    Require(batch.frames[n].stamp.mic_sample == n * 160 &&
            batch.frames[n].stamp.reference_sample == n * 160 &&
            batch.frames[n].stamp.delivered_ns == 1000000000,
            "batch pair/delivery stamps mismatch");
    auto cleaned = aec.Process(batch.frames[n], 0);
    auto voice = vad.Analyze(cleaned.cleaned);
    Steps("90MS_SYNTHETIC_ZERO_HINT_FRAME", aec, &voice);
    Require(cleaned.health.healthy() && voice.process_status == 0 &&
            voice.voice == Voice::kNonVoice, "90ms real backend pair failed");
  }
  PairedWebrtcAec off_aec; WebrtcVoiceDetector off_vad;
  ShadowVoiceCandidate off(off_aec, off_vad); off.Begin(kContext);
  auto off_result = off.Push(Chunk(bytes, 0, 1000000000));
  Require(!off.enabled() && !off_aec.initialization().created &&
          !off_vad.initialization().created && !off_result.paired_frames,
          "default-off path allocated/called real APM");
  PairedWebrtcAec unknown_aec; WebrtcVoiceDetector unknown_vad;
  ShadowVoiceCandidate enabled(unknown_aec, unknown_vad, true); enabled.Begin(kContext);
  auto result = enabled.Push(Chunk(bytes, 0, 1000000000));
  Require(result.aec_rejected == 1 && result.discarded_frames == 8 &&
          result.candidate_events == 0 && !result.last_candidate &&
          enabled.cached_pcm_bytes() == 0 && unknown_vad.successful_frames() == 0,
          "UNKNOWN delay reached VAD/cache/candidate");
  std::cout << "PASS 90ms_9_real_pairs default_off_no_APM UNKNOWN_fail_closed_no_candidate\n";
  // The final runner binds these tests to the separately reviewed shadow fix.
}
}  // namespace

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "expected synthetic evidence output directory");
    std::cout << "APM_0.3_REAL_ARM_QEMU_INTERFACE_TEST_SYNTHETIC_ONLY\n";
    FailureBoundaries(); RealNonfiniteBoundary(); Synthetic(argv[1]); BatchesAndShadow();
    FailedInitializationLatch();
    std::cout << "PASS real_APM_adapter_interfaces PRODUCT_AND_ACOUSTICS_NOT_RUN\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
