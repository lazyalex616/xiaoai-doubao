#include "audio/shadow_voice.hpp"

#include <limits>

namespace xiaoai_plus::audio::shadow {

bool Context::valid() const {
  return epoch && session && response && playback && ingress && capture;
}
bool Context::operator==(const Context& b) const {
  return epoch == b.epoch && session == b.session && response == b.response &&
         playback == b.playback && ingress == b.ingress && capture == b.capture;
}
bool LocalInputAdapter::Begin(const Context& context) {
  if (!context.valid() || context.capture < capture_frontier_) {
    context_ = {};
    ClearContinuity();
    return false;
  }
  if (context.capture == capture_frontier_) {
    if (faulted_) return false;
    if (context != context_) { partial_ = {}; pending_ = 0; }
    context_ = context;
    return true;
  }
  // The owner must attest an actual acquisition restart, not relabel old PCM.
  context_ = context;
  capture_frontier_ = context.capture;
  faulted_ = false;
  partial_ = {};
  pending_ = 0;
  has_delivery_ = false;
  next_sample_ = last_delivery_ns_ = 0;
  return true;
}
void LocalInputAdapter::ClearContinuity() {
  partial_ = {};
  pending_ = 0;
  faulted_ = true;
}
PairedBatch LocalInputAdapter::Push(const LocalChunk& chunk) {
  PairedBatch out;
  const size_t samples = chunk.bytes / 8;
  if (!context_.valid()) out.fault = InputFault::kUnbound;
  else if (chunk.context != context_) out.fault = InputFault::kContext;
  else if (faulted_) out.fault = InputFault::kCaptureFault;
  else if (!chunk.reference_present) out.fault = InputFault::kMissingReference;
  else if (!chunk.pcm || !chunk.bytes || chunk.bytes % 8 || !chunk.delivered_ns)
    out.fault = InputFault::kMalformed;
  else if (samples > 1440 ||
           chunk.mic_sample > std::numeric_limits<uint64_t>::max() - samples)
    out.fault = InputFault::kOverflow;
  else if (chunk.mic_sample != chunk.reference_sample ||
           (has_delivery_ && chunk.mic_sample != next_sample_))
    out.fault = InputFault::kSequence;
  else if (has_delivery_ && chunk.delivered_ns < last_delivery_ns_)
    out.fault = InputFault::kDeliveryGap;
  if (out.fault != InputFault::kNone) { ClearContinuity(); return out; }

  auto read = [](const uint8_t* p) {
    const uint16_t bits = uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    return static_cast<int16_t>(bits < 32768 ? int(bits) : int(bits) - 65536);
  };
  for (size_t i = 0; i < samples; ++i) {
    if (pending_ == 0) {
      partial_.stamp = {context_, chunk.mic_sample + i,
                        chunk.reference_sample + i, chunk.delivered_ns};
    }
    partial_.mic[pending_] = read(chunk.pcm + i * 8);
    partial_.reference[pending_] = read(chunk.pcm + i * 8 + 6);
    if (++pending_ == kSamplesPerFrame) {
      partial_.stamp.delivered_ns = chunk.delivered_ns;
      out.frames[out.count++] = partial_;
      partial_ = {};
      pending_ = 0;
    }
  }
  has_delivery_ = true;
  next_sample_ = chunk.mic_sample + samples;
  last_delivery_ns_ = chunk.delivered_ns;
  return out;
}
bool AecHealth::healthy() const {
  return initialized && reverse_processed && capture_processed &&
         reverse_status == 0 && capture_status == 0 &&
         (!delay_applied || delay_status == 0);
}
ShadowVoiceCandidate::ShadowVoiceCandidate(PairedAec& aec,
                                         VoiceActivityDetector& vad, bool enabled)
    : aec_(aec), vad_(vad), enabled_(enabled) {}
void ShadowVoiceCandidate::ClearWindow() {
  pcm_cache_ = {};
  stamps_ = {};
  cached_ = write_ = voiced_frames_ = 0;
  streak_.reset();
  candidate_.reset();
}
void ShadowVoiceCandidate::LatchFault() {
  input_.ClearContinuity();
  ClearWindow();
  aec_ready_ = vad_ready_ = false;
}
bool ShadowVoiceCandidate::Begin(const Context& context) {
  if (!input_.Begin(context)) {
    LatchFault();
    return false;
  }
  if (context == context_) return aec_ready_ && vad_ready_;
  context_ = context;
  ClearWindow();
  // Disabled monitoring is intentional, not an initialization fault.
  if (!enabled_) { aec_ready_ = vad_ready_ = false; return false; }
  aec_ready_ = aec_.kind() != BackendKind::kUnavailable && aec_.Reset();
  if (!aec_ready_) { LatchFault(); return false; }
  vad_ready_ = vad_.kind() != BackendKind::kUnavailable && vad_.Reset();
  if (!vad_ready_) { LatchFault(); return false; }
  return true;
}
void ShadowVoiceCandidate::Expire(uint64_t now) {
  if (!cached_) return;
  const size_t oldest = (write_ + kCacheFrames - cached_) % kCacheFrames;
  const auto at = stamps_[oldest].delivered_ns;
  if (now < at || now - at > kWindowNs) ClearWindow();
}
ShadowResult ShadowVoiceCandidate::Push(const LocalChunk& chunk) {
  ShadowResult result;
  if (!enabled_) return result;
  const auto batch = input_.Push(chunk);
  result.input_fault = batch.fault;
  result.paired_frames = batch.count;
  if (batch.fault != InputFault::kNone) {
    LatchFault();
    return result;
  }
  Expire(chunk.delivered_ns);
  for (size_t i = 0; i < batch.count; ++i) {
    if (!Process(batch.frames[i], result)) {
      // Discard the rest of this delivery and its partial frame. A processing
      // fault must not let post-fault PCM silently bridge a new capture anchor.
      result.discarded_frames = batch.count - i - 1;
      result.candidate_events = 0;
      result.last_candidate.reset();
      break;
    }
  }
  return result;
}
bool ShadowVoiceCandidate::Process(const PairedFrame& frame, ShadowResult& result) {
  if (!aec_ready_ || aec_.kind() == BackendKind::kUnavailable) {
    ++result.aec_rejected;
    LatchFault();
    return false;
  }
  const auto processed = aec_.Process(frame, std::nullopt);
  if (!processed.health.healthy()) {
    ++result.aec_rejected;
    LatchFault();
    return false;
  }
  if (!vad_ready_ || vad_.kind() == BackendKind::kUnavailable) {
    ++result.vad_rejected;
    LatchFault();
    return false;
  }
  const auto detected = vad_.Analyze(processed.cleaned);
  if (!detected.initialized || detected.process_status != 0 ||
      detected.voice == Voice::kUnknown) {
    ++result.vad_rejected;
    LatchFault();
    return false;
  }
  // Never retain a candidate after its starting PCM has left the bounded ring.
  if (cached_ == kCacheFrames && streak_ &&
      streak_->start_sample <= stamps_[write_].mic_sample) ClearWindow();
  pcm_cache_[write_] = processed.cleaned;
  stamps_[write_] = frame.stamp;
  write_ = (write_ + 1) % kCacheFrames;
  if (cached_ < kCacheFrames) ++cached_;
  if (detected.voice == Voice::kNonVoice) {
    voiced_frames_ = 0;
    streak_.reset();
    candidate_.reset();
    return true;
  }
  if (!streak_) {
    streak_ = Candidate{context_, frame.stamp.mic_sample,
                        frame.stamp.reference_sample, frame.stamp.delivered_ns,
                        aec_.kind(), vad_.kind()};
  }
  // Three injected voice frames exercise continuity; 30 ms is a provisional
  // shadow observation threshold, not a validated acoustic speech criterion.
  if (++voiced_frames_ == 3) {
    candidate_ = streak_;
    result.last_candidate = candidate_;
    ++result.candidate_events;
  }
  return true;
}

}  // namespace xiaoai_plus::audio::shadow
