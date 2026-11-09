#include "audio/shadow_webrtc.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <webrtc/modules/audio_processing/include/audio_processing.h>

namespace xiaoai_plus::audio::shadow {
namespace {
using Floats = std::array<float, kSamplesPerFrame>;
bool Supported(const ApmFormat& format) {
  return format.sample_rate_hz == 16000 && format.channels == 1;
}
webrtc::ProcessingConfig ProcessingFormat() {
  webrtc::ProcessingConfig config;
  for (auto& stream : config.streams) stream = webrtc::StreamConfig(16000, 1, false);
  return config;
}
bool OtherComponentsDisabled(webrtc::AudioProcessing& p) {
  return !p.echo_control_mobile()->is_enabled() && !p.gain_control()->is_enabled() &&
         !p.high_pass_filter()->is_enabled() && !p.level_estimator()->is_enabled() &&
         !p.noise_suppression()->is_enabled();
}
void Normalize(const PcmFrame& from, Floats& to) {
  for (size_t i = 0; i < from.size(); ++i) to[i] = float(from[i]) / 32768.0f;
}
bool Quantize(const Floats& from, PcmFrame& to) {
  for (float sample : from) if (!std::isfinite(sample)) return false;
  for (size_t i = 0; i < from.size(); ++i) {
    const float sample = std::max(-1.0f, std::min(1.0f, from[i]));
    const long quantized = std::lround(sample * 32768.0f);
    to[i] = static_cast<int16_t>(std::max(-32768L, std::min(32767L, quantized)));
  }
  return true;
}
}  // namespace

struct PairedWebrtcAec::Impl {
  Impl(ApmFormat configured, bool extended) : format(configured), extended_filter(extended) {}
  const ApmFormat format;
  const bool extended_filter;
  std::unique_ptr<webrtc::AudioProcessing> apm;
  ApmInitReceipt init;
  AecStepReceipt step;
  Floats mic{}, reference{}, residual{}, reverse_output{};
  Context context{};
  uint64_t next_sample{0}, last_delivery_ns{0};
  bool bound{false};
  size_t successful{0};
};
PairedWebrtcAec::PairedWebrtcAec(ApmFormat format, bool extended_filter)
    : impl_(std::make_unique<Impl>(format, extended_filter)) {}
PairedWebrtcAec::~PairedWebrtcAec() = default;
const ApmInitReceipt& PairedWebrtcAec::initialization() const { return impl_->init; }
const AecStepReceipt& PairedWebrtcAec::last_step() const { return impl_->step; }
size_t PairedWebrtcAec::successful_pairs() const { return impl_->successful; }
bool PairedWebrtcAec::Reset() {
  auto& p = *impl_;
  p.apm.reset(); p.init = {}; p.step = {};
  p.mic = {}; p.reference = {}; p.residual = {}; p.reverse_output = {};
  p.context = {}; p.next_sample = p.last_delivery_ns = 0;
  p.bound = false; p.successful = 0;
  p.init.format_status = Supported(p.format) ? 0 : kUnsupportedShadowFormat;
  if (p.init.format_status != 0) return false;
  webrtc::Config options;
  options.Set<webrtc::DelayAgnostic>(new webrtc::DelayAgnostic(true));
  options.Set<webrtc::ExtendedFilter>(new webrtc::ExtendedFilter(p.extended_filter));
  p.apm.reset(webrtc::AudioProcessing::Create(options));
  p.init.created = bool(p.apm);
  if (!p.apm) return false;
  p.init.enable_status = p.apm->echo_cancellation()->Enable(true);
  p.init.tuning_status = p.apm->echo_cancellation()->set_suppression_level(
      webrtc::EchoCancellation::kHighSuppression);
  p.init.initialize_status = p.apm->Initialize(ProcessingFormat());
  p.init.only_requested_component = OtherComponentsDisabled(*p.apm) &&
      !p.apm->voice_detection()->is_enabled() && p.apm->echo_cancellation()->is_enabled();
  p.init.ready = p.init.enable_status == 0 && p.init.tuning_status == 0 &&
      p.init.initialize_status == 0 && p.init.only_requested_component;
  return p.init.ready;
}
AecResult PairedWebrtcAec::Process(const PairedFrame& frame, std::optional<int> delay) {
  auto& p = *impl_;
  p.step = {}; p.step.processing_io_delay_hint_ms = delay;
  AecResult out;  // Never returns raw PCM on an error, including partial output.
  out.health.initialized = p.init.ready;
  auto finish = [&]() {
    p.step.health = out.health;
    if (!out.health.healthy()) p.init.ready = false;
    return out;
  };
  if (!p.init.ready || !p.apm) return finish();
  const auto& stamp = frame.stamp;
  const bool valid = stamp.context.valid() && stamp.delivered_ns &&
      stamp.mic_sample == stamp.reference_sample &&
      stamp.mic_sample <= std::numeric_limits<uint64_t>::max() - kSamplesPerFrame &&
      (!p.bound || (stamp.context == p.context && stamp.mic_sample == p.next_sample &&
          stamp.delivered_ns >= p.last_delivery_ns));
  p.step.pair_status = valid ? 0 : kInvalidPair;
  if (!valid) return finish();
  p.context = stamp.context; p.bound = true;
  p.next_sample = stamp.mic_sample + kSamplesPerFrame;
  p.last_delivery_ns = stamp.delivered_ns;
  Normalize(frame.mic, p.mic); Normalize(frame.reference, p.reference);
  p.residual = {}; p.reverse_output = {};
  const float* reference[] = {p.reference.data()};
  float* reverse[] = {p.reverse_output.data()};
  const auto format = webrtc::StreamConfig(16000, 1, false);
  out.health.reverse_processed = true;
  out.health.reverse_status = p.apm->ProcessReverseStream(reference, format, format, reverse);
  if (out.health.reverse_status != 0) return finish();
  if (delay) {
    out.health.delay_applied = true;
    out.health.delay_status = p.apm->set_stream_delay_ms(*delay);
    // Even public warning -13 is a rejection; do not hide truncation.
    if (out.health.delay_status != 0) return finish();
  }
  const float* mic[] = {p.mic.data()}; float* residual[] = {p.residual.data()};
  out.health.capture_processed = true;
  out.health.capture_status = p.apm->ProcessStream(mic, format, format, residual);
  p.step.capture_api_status = out.health.capture_status;
  if (out.health.capture_status != 0) return finish();
  if (!Quantize(p.residual, out.cleaned)) {
    for (float sample : p.residual) if (!std::isfinite(sample)) ++p.step.nonfinite_residual_samples;
    out.health.capture_status = kNonFiniteResidual;
    return finish();
  }
  p.step.residual_valid = true;
  ++p.successful;
  return finish();
}

struct WebrtcVoiceDetector::Impl {
  explicit Impl(ApmFormat configured) : format(configured) {}
  const ApmFormat format;
  std::unique_ptr<webrtc::AudioProcessing> apm;
  ApmInitReceipt init;
  VadResult result;
  Floats input{}, output{};
  size_t successful{0};
};
WebrtcVoiceDetector::WebrtcVoiceDetector(ApmFormat format) : impl_(std::make_unique<Impl>(format)) {}
WebrtcVoiceDetector::~WebrtcVoiceDetector() = default;
const ApmInitReceipt& WebrtcVoiceDetector::initialization() const { return impl_->init; }
const VadResult& WebrtcVoiceDetector::last_result() const { return impl_->result; }
size_t WebrtcVoiceDetector::successful_frames() const { return impl_->successful; }
bool WebrtcVoiceDetector::Reset() {
  auto& p = *impl_;
  p.apm.reset(); p.init = {}; p.result = {}; p.input = {}; p.output = {}; p.successful = 0;
  p.init.format_status = Supported(p.format) ? 0 : kUnsupportedShadowFormat;
  if (p.init.format_status != 0) return false;
  p.apm.reset(webrtc::AudioProcessing::Create());
  p.init.created = bool(p.apm);
  if (!p.apm) return false;
  p.init.enable_status = p.apm->voice_detection()->Enable(true);
  p.init.tuning_status = p.apm->voice_detection()->set_frame_size_ms(10);
  p.init.likelihood_status = p.apm->voice_detection()->set_likelihood(
      webrtc::VoiceDetection::kModerateLikelihood);
  p.init.vad_frame_size_ms = p.apm->voice_detection()->frame_size_ms();
  p.init.vad_likelihood = p.apm->voice_detection()->likelihood();
  p.init.initialize_status = p.apm->Initialize(ProcessingFormat());
  p.init.only_requested_component = OtherComponentsDisabled(*p.apm) &&
      !p.apm->echo_cancellation()->is_enabled() && p.apm->voice_detection()->is_enabled();
  p.init.ready = p.init.enable_status == 0 && p.init.tuning_status == 0 &&
      p.init.likelihood_status == 0 && p.init.vad_frame_size_ms == 10 &&
      p.init.vad_likelihood == webrtc::VoiceDetection::kModerateLikelihood &&
      p.init.initialize_status == 0 && p.init.only_requested_component;
  return p.init.ready;
}
VadResult WebrtcVoiceDetector::Analyze(const PcmFrame& cleaned) {
  auto& p = *impl_;
  p.result = {}; p.result.initialized = p.init.ready;
  if (!p.init.ready || !p.apm) return p.result;
  Normalize(cleaned, p.input); p.output = {};
  const float* input[] = {p.input.data()}; float* output[] = {p.output.data()};
  const auto format = webrtc::StreamConfig(16000, 1, false);
  p.result.process_status = p.apm->ProcessStream(input, format, format, output);
  if (p.result.process_status != 0) { p.init.ready = false; return p.result; }
  p.result.voice = p.apm->voice_detection()->stream_has_voice() ? Voice::kVoice : Voice::kNonVoice;
  ++p.successful;
  return p.result;
}
}  // namespace xiaoai_plus::audio::shadow
