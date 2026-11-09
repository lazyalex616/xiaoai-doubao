#include "audio/shadow_voice.hpp"

#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

using namespace xiaoai_plus::audio::shadow;
const Context kContext{41, 2, 3, 4, 5, 6};

std::vector<uint8_t> Source(size_t count, uint64_t first = 0) {
  std::vector<uint8_t> out(count * 8);
  for (size_t i = 0; i < count; ++i) {
    const int16_t values[]{int16_t(-16000 + (first + i) % 30000), 111, 222,
                           int16_t(16000 - (first + i) % 30000)};
    for (size_t ch = 0; ch < 4; ++ch) {
      const uint16_t bits = static_cast<uint16_t>(values[ch]);
      out[i * 8 + ch * 2] = uint8_t(bits);
      out[i * 8 + ch * 2 + 1] = uint8_t(bits >> 8);
    }
  }
  return out;
}
LocalChunk Chunk(const std::vector<uint8_t>& data, uint64_t first = 0,
                 uint64_t at = 1000000000, Context context = kContext) {
  return {context, first, first, at, data.data(), data.size(), true};
}

// Explicit INJECTED FIXTURE. No acoustic AEC, no actual VAD dependency.
struct FixtureAec final : PairedAec {
  BackendKind backend{BackendKind::kInjectedFixture};
  bool ready{true};
  AecHealth health{true, true, true, 0, 0, false, -1};
  size_t resets{0}, calls{0};
  size_t fail_on_call{0};
  std::vector<uint64_t> observed;
  BackendKind kind() const override { return backend; }
  bool Reset() override { ++resets; return ready; }
  AecResult Process(const PairedFrame& frame, std::optional<int> delay) override {
    assert(!delay);  // unknown acoustic delay must never silently become zero
    assert(frame.stamp.mic_sample == frame.stamp.reference_sample);
    ++calls;
    observed.push_back(frame.stamp.mic_sample);
    AecResult out;
    out.health = health;
    if (calls == fail_on_call) out.health.capture_status = -9;
    out.cleaned.fill(777);  // proves the VAD receives processed, not raw PCM
    return out;
  }
};
struct FixtureVad final : VoiceActivityDetector {
  BackendKind backend{BackendKind::kInjectedFixture};
  bool ready{true};
  VadResult answer{true, 0, Voice::kVoice};
  size_t calls{0}, resets{0};
  BackendKind kind() const override { return backend; }
  bool Reset() override { ++resets; return ready; }
  VadResult Analyze(const PcmFrame& cleaned) override {
    ++calls;
    for (auto value : cleaned) assert(value == 777);
    return answer;
  }
};

void Pairing() {
  LocalInputAdapter input;
  input.Begin(kContext);
  const auto source = Source(1440, 1000);
  const auto batch = input.Push(Chunk(source, 1000));
  assert(batch.fault == InputFault::kNone && batch.count == 9);
  for (size_t f = 0; f < 9; ++f) {
    const auto& pair = batch.frames[f];
    assert(pair.stamp.context == kContext);
    assert(pair.stamp.mic_sample == 1000 + f * 160);
    assert(pair.stamp.reference_sample == pair.stamp.mic_sample);
    assert(pair.stamp.delivered_ns == 1000000000);
    for (size_t s = 0; s < 160; ++s) {
      assert(pair.mic[s] == -16000 + 1000 + int(f * 160 + s));
      assert(pair.reference[s] == 16000 - 1000 - int(f * 160 + s));
    }
  }
  auto restarted = kContext; ++restarted.capture;
  assert(input.Begin(restarted));
  const auto first = Source(87), second = Source(73, 87);
  assert(input.Push(Chunk(first, 0, 1000000000, restarted)).count == 0 && input.pending_samples() == 87);
  const auto joined = input.Push(Chunk(second, 87, 1010000000, restarted));
  assert(joined.count == 1 && input.pending_samples() == 0);
  assert(joined.frames[0].stamp.mic_sample == 0);
  assert(joined.frames[0].stamp.delivered_ns == 1010000000);
  for (size_t i = 0; i < 160; ++i) assert(joined.frames[0].mic[i] == -16000 + int(i));
  std::cout << "PASS exact_90ms_to_9_pairs_and_split_signed_pcm_delivery_stamp\n";
}

void InputFailures() {
  const auto frame = Source(160), partial = Source(80), large = Source(1441);
  for (int which = 0; which < 8; ++which) {
    FixtureAec aec; FixtureVad vad;
    ShadowVoiceCandidate shadow(aec, vad, true);
    shadow.Begin(kContext);
    assert(shadow.Push(Chunk(frame)).candidate_events == 0);
    assert(shadow.Push(Chunk(frame, 160, 1010000000)).candidate_events == 0);
    auto broken = Chunk(frame, 320, 1020000000);
    if (which == 0) broken.reference_present = false;
    if (which == 1) ++broken.reference_sample;
    if (which == 2) ++broken.mic_sample, ++broken.reference_sample;
    if (which == 3) --broken.bytes;
    if (which == 4) broken = Chunk(large, 320, 1020000000);
    if (which == 5) broken.delivered_ns = 1210000001;
    if (which == 6) broken.delivered_ns = 999999999;
    if (which == 7) broken.mic_sample = broken.reference_sample =
                       std::numeric_limits<uint64_t>::max() - 159;
    const auto result = shadow.Push(broken);
    if(which==5) { assert(result.input_fault==InputFault::kNone && aec.calls==3); continue; }
    assert(result.input_fault != InputFault::kNone);
    assert(!result.candidate_events && !result.last_candidate);
    assert(shadow.cached_pcm_bytes() == 0 && !shadow.candidate());
    assert(aec.calls == 2 && vad.calls == 2);
    const auto resets = aec.resets;
    assert(!shadow.Begin(kContext));
    assert(shadow.Push(Chunk(frame, 5000, 1300000000)).input_fault == InputFault::kCaptureFault);
    assert(aec.calls == 2 && aec.resets == resets);
    auto restarted = kContext; ++restarted.capture;
    assert(shadow.Begin(restarted));
    assert(shadow.Push(Chunk(frame, 0, 1300000000, restarted)).candidate_events == 0);
    assert(shadow.Push(Chunk(frame, 160, 1310000000, restarted)).candidate_events == 0);
    assert(shadow.Push(Chunk(frame, 320, 1320000000, restarted)).candidate_events == 1);
  }
  LocalInputAdapter input; input.Begin(kContext);
  assert(input.Push(Chunk(partial)).count == 0);
  auto bad = Chunk(partial, 80, 1010000000); bad.reference_present = false;
  assert(input.Push(bad).fault == InputFault::kMissingReference);
  assert(input.pending_samples() == 0);
  assert(input.Push(Chunk(partial, 160, 1020000000)).fault == InputFault::kCaptureFault);
  std::cout << "PASS reference_sequence_size_overflow_delivery_faults_reset_without_frankenstein_pcm\n";
}

void HealthFailures() {
  const auto frame = Source(160);
  for (int which = 0; which < 8; ++which) {
    FixtureAec aec; FixtureVad vad;
    ShadowVoiceCandidate shadow(aec, vad, true); shadow.Begin(kContext);
    for (int i = 0; i < 2; ++i)
      assert(shadow.Push(Chunk(frame, i * 160, 1000000000 + i * 10000000)).candidate_events == 0);
    if (which == 0) aec.health.initialized = false;
    if (which == 1) aec.health.reverse_processed = false;
    if (which == 2) aec.health.capture_processed = false;
    if (which == 3) aec.health.reverse_status = -3;
    if (which == 4) aec.health.capture_status = 1;
    if (which == 5) aec.health.delay_applied = true, aec.health.delay_status = -4;
    if (which == 6) vad.answer.initialized = false;
    if (which == 7) vad.answer.process_status = -5;
    const auto rejected = shadow.Push(Chunk(frame, 320, 1020000000));
    assert(rejected.candidate_events == 0 && !shadow.candidate());
    assert(shadow.cached_pcm_bytes() == 0);
    assert(which < 6 ? rejected.aec_rejected == 1 : rejected.vad_rejected == 1);
    if (which < 6) assert(vad.calls == 2);  // no failed/raw fallback frame enters VAD
    aec.health = {true, true, true, 0, 0, false, -1};
    vad.answer = {true, 0, Voice::kVoice};
    assert(!shadow.Begin(kContext));
    assert(shadow.Push(Chunk(frame, 480, 1030000000)).input_fault == InputFault::kCaptureFault);
    auto restarted = kContext; ++restarted.capture;
    assert(shadow.Begin(restarted));
    assert(shadow.Push(Chunk(frame, 0, 1030000000, restarted)).candidate_events == 0);
    assert(shadow.Push(Chunk(frame, 160, 1040000000, restarted)).candidate_events == 0);
    const auto observed = shadow.Push(Chunk(frame, 320, 1050000000, restarted));
    assert(observed.candidate_events == 1);
    assert(observed.last_candidate->start_sample == 0);
    assert(observed.last_candidate->context == restarted);
    assert(observed.last_candidate->vad_kind == BackendKind::kInjectedFixture);
  }
  FixtureAec aec; FixtureVad vad;
  ShadowVoiceCandidate unknown(aec, vad, true); unknown.Begin(kContext);
  vad.answer.voice = Voice::kUnknown;
  assert(unknown.Push(Chunk(frame)).vad_rejected == 1);
  assert(unknown.cached_pcm_bytes() == 0);
  // Failure with one complete frame plus partial PCM must discard both, and
  // the next capture anchor must start a new streak rather than bridge it.
  FixtureAec partial_aec; FixtureVad partial_vad;
  ShadowVoiceCandidate partial(partial_aec, partial_vad, true); partial.Begin(kContext);
  partial_aec.health.capture_status = -1;
  assert(partial.Push(Chunk(Source(240))).aec_rejected == 1);
  partial_aec.health.capture_status = 0;
  auto restarted = kContext; ++restarted.capture;
  assert(partial.Begin(restarted));
  const auto half = partial.Push(Chunk(Source(80), 0, 1100000000, restarted));
  assert(half.paired_frames == 0 && partial.cached_frames() == 0);
  assert(partial.Push(Chunk(Source(80), 80, 1110000000, restarted)).paired_frames == 1);
  assert(partial_aec.observed.back() == 0);
  FixtureAec mixed_aec; FixtureVad mixed_vad;
  mixed_aec.fail_on_call = 4;
  ShadowVoiceCandidate mixed(mixed_aec, mixed_vad, true); mixed.Begin(kContext);
  const auto mixed_result = mixed.Push(Chunk(Source(1440)));
  assert(mixed_result.aec_rejected == 1 && mixed_result.discarded_frames == 5);
  assert(mixed_result.candidate_events == 0 && !mixed_result.last_candidate);
  assert(mixed.cached_frames() == 0 && mixed_aec.calls == 4 && mixed_vad.calls == 3);
  std::cout << "PASS aec_init_reverse_delay_capture_and_vad_error_fail_closed_provenance_is_injected\n";
}

void Boundaries() {
  const auto frame = Source(160);
  FixtureAec aec; FixtureVad vad;
  ShadowVoiceCandidate off(aec, vad); off.Begin(kContext);
  assert(!off.enabled() && off.Push(Chunk(Source(1440))).candidate_events == 0);
  assert(aec.resets == 0 && aec.calls == 0 && vad.resets == 0 && vad.calls == 0);
  ShadowVoiceCandidate shadow(aec, vad, true); shadow.Begin(kContext);
  vad.answer.voice = Voice::kNonVoice;
  for (int i = 0; i < 50; ++i) {
    const auto out = shadow.Push(Chunk(frame, i * 160, 1000000000 + i * 10000000));
    assert(out.candidate_events == 0);
    assert(shadow.cached_frames() <= 20 && shadow.cached_pcm_bytes() <= 6400);
  }
  assert(shadow.cached_frames() == 20);
  shadow.Expire(1700000001);
  assert(shadow.cached_pcm_bytes() == 0);
  vad.answer.voice = Voice::kVoice;
  auto restarted = kContext; ++restarted.capture;
  assert(shadow.Begin(restarted));
  for (int i = 0; i < 20; ++i) shadow.Push(Chunk(frame, i * 160, 2000000000, restarted));
  assert(shadow.candidate() && shadow.candidate()->start_sample == 0);
  assert(shadow.cached_pcm_bytes() == 6400);
  shadow.Push(Chunk(frame, 3200, 2000000000, restarted));
  assert(!shadow.candidate());  // candidate origin evicted, cannot hold it forever
  shadow.Expire(2200000001);
  assert(shadow.cached_pcm_bytes() == 0);
  for (int field = 0; field < 6; ++field) {
    auto next = kContext;
    if (field == 0) ++next.epoch;
    if (field == 1) ++next.session;
    if (field == 2) ++next.response;
    if (field == 3) ++next.playback;
    if (field == 4) ++next.ingress;
    if (field == 5) ++next.capture;
    ShadowVoiceCandidate fenced(aec, vad, true); assert(fenced.Begin(next));
    assert(fenced.cached_pcm_bytes() == 0 && !fenced.candidate());
    const auto fresh = fenced.Push(Chunk(frame, 0, 3000000000, next));
    assert(fresh.candidate_events == 0 && fenced.cached_frames() == 1);
    auto stale = Chunk(frame);
    assert(fenced.Push(stale).input_fault == InputFault::kContext);
    assert(fenced.cached_pcm_bytes() == 0 && !fenced.candidate());
  }
  shadow.Begin({});
  assert(shadow.Push(Chunk(frame)).input_fault == InputFault::kUnbound);
  FixtureAec missing; FixtureVad detector;
  missing.backend = BackendKind::kUnavailable;
  ShadowVoiceCandidate unavailable(missing, detector, true); unavailable.Begin(kContext);
  const auto no = unavailable.Push(Chunk(Source(1440)));
  assert(no.input_fault == InputFault::kCaptureFault && no.paired_frames == 0 && no.candidate_events == 0);
  assert(missing.calls == 0 && detector.calls == 0);
  FixtureAec failed; failed.ready = false;
  ShadowVoiceCandidate init_failed(failed, detector, true); init_failed.Begin(kContext);
  assert(init_failed.Push(Chunk(frame)).input_fault == InputFault::kCaptureFault && failed.calls == 0);
  FixtureAec working; FixtureVad unavailable_vad;
  unavailable_vad.backend = BackendKind::kUnavailable;
  ShadowVoiceCandidate missing_vad(working, unavailable_vad, true); missing_vad.Begin(kContext);
  assert(missing_vad.Push(Chunk(frame)).input_fault == InputFault::kCaptureFault && unavailable_vad.calls == 0);
  FixtureVad failed_vad; failed_vad.ready = false;
  ShadowVoiceCandidate vad_init_failed(working, failed_vad, true); vad_init_failed.Begin(kContext);
  assert(vad_init_failed.Push(Chunk(frame)).input_fault == InputFault::kCaptureFault && failed_vad.calls == 0);
  std::cout << "PASS default_off_no_backend_calls_bounded_cache_candidate_origin_expiry_all_context_fences\n";
  std::cout << "NOT_RUN real_acoustic_AEC_VAD_child_echo_provider_performance_first_word\n";
}
void RecoveryContract() {
  const auto frame = Source(160);
  FixtureAec aec; FixtureVad vad;
  ShadowVoiceCandidate shadow(aec, vad, true); assert(shadow.Begin(kContext));
  shadow.Push(Chunk(frame));
  const auto resets = aec.resets;
  assert(shadow.Begin(kContext));  // idempotent, cannot rewind already delivered PCM
  assert(aec.resets == resets);
  assert(shadow.Push(Chunk(frame, 160, 1010000000)).input_fault == InputFault::kNone);
  auto response = kContext; ++response.response;
  assert(shadow.Begin(response));  // same physical acquisition, watermark survives rebind
  const auto calls = aec.calls;
  assert(shadow.Push(Chunk(frame, 0, 1020000000, response)).input_fault == InputFault::kSequence);
  assert(aec.calls == calls);
  auto different = response; ++different.session;
  assert(!shadow.Begin(different));  // response/session relabeling cannot unlock a fault
  assert(shadow.Push(Chunk(frame, 320, 1030000000, response)).input_fault == InputFault::kCaptureFault);
  auto restarted = different; ++restarted.capture;
  assert(shadow.Begin(restarted));
  assert(shadow.Push(Chunk(frame, 0, 1040000000, restarted)).candidate_events == 0);
  assert(shadow.Push(Chunk(frame, 160, 1050000000, restarted)).candidate_events == 0);
  const auto recovered = shadow.Push(Chunk(frame, 320, 1060000000, restarted));
  assert(recovered.candidate_events == 1 && recovered.last_candidate->context == restarted);
  assert(!shadow.Begin(kContext));  // capture numbers cannot move backwards
  assert(!shadow.Begin(restarted));
  ++restarted.capture;
  assert(shadow.Begin(restarted));
  LocalInputAdapter direct; assert(direct.Begin(kContext));
  direct.Push(Chunk(frame)); direct.ClearContinuity();
  assert(!direct.Begin(kContext));
  assert(direct.Push(Chunk(frame, 160, 1070000000)).fault == InputFault::kCaptureFault);
  assert(direct.Begin(restarted));
  std::cout << "PASS fault_latch_requires_strictly_new_physical_capture_idempotent_begin_and_same_capture_rebind_keep_watermark\n";
}
void InitializationContract() {
  for (int failure = 0; failure < 4; ++failure) {
    FixtureAec aec; FixtureVad vad;
    if (failure == 0) aec.ready = false;
    if (failure == 1) vad.ready = false;
    if (failure == 2) aec.backend = BackendKind::kUnavailable;
    if (failure == 3) vad.backend = BackendKind::kUnavailable;
    ShadowVoiceCandidate shadow(aec, vad, true);
    assert(!shadow.Begin(kContext));
    const auto ar = aec.resets, vr = vad.resets;
    aec.ready = vad.ready = true;
    aec.backend = vad.backend = BackendKind::kInjectedFixture;
    // No intervening Push: initialization itself must already quarantine the
    // capture before any same-capture owner relabel can try another Reset.
    for (int field = 0; field < 5; ++field) {
      auto changed = kContext;
      if (field == 0) ++changed.epoch;
      if (field == 1) ++changed.session;
      if (field == 2) ++changed.response;
      if (field == 3) ++changed.playback;
      if (field == 4) ++changed.ingress;
      assert(!shadow.Begin(changed));
      assert(aec.resets == ar && vad.resets == vr && aec.calls == 0 && vad.calls == 0);
    }
    assert(shadow.Push(Chunk(Source(1440))).input_fault == InputFault::kCaptureFault);
    auto restarted = kContext; ++restarted.capture;
    assert(shadow.Begin(restarted));
    const auto recovered = shadow.Push(Chunk(Source(1440), 0, 1100000000, restarted));
    assert(recovered.candidate_events == 1 && recovered.last_candidate->context == restarted);
    assert(aec.calls == 9 && vad.calls == 9);
  }
  FixtureAec off_aec; FixtureVad off_vad;
  off_aec.backend = off_vad.backend = BackendKind::kUnavailable;
  ShadowVoiceCandidate off(off_aec, off_vad);
  assert(!off.Begin(kContext));
  auto rebind = kContext; ++rebind.session;
  assert(!off.Begin(rebind));
  assert(off_aec.resets == 0 && off_vad.resets == 0 && off_aec.calls == 0 && off_vad.calls == 0);
  std::cout << "PASS failed_or_unavailable_aec_vad_begin_latches_before_push_all_owner_relabels_new_capture_recovers_default_off_no_backend\n";
}
int main() { Pairing(); InputFailures(); HealthFailures(); Boundaries(); RecoveryContract(); InitializationContract(); }
