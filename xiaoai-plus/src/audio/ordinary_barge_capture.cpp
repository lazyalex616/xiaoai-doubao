#include "audio/ordinary_barge_capture.hpp"
#include <cmath>

namespace xiaoai_plus::audio {
OrdinaryBargeCapture::OrdinaryBargeCapture(shadow::PairedAec& aec,
    shadow::VoiceActivityDetector& vad, int hint) : aec_(aec), vad_(vad), hint_(hint) {}
float OrdinaryBargeCapture::ReferenceAt(int64_t sample) const {
  if(sample<0 || uint64_t(sample)>=history_samples_ || history_samples_-uint64_t(sample)>reference_history_.size())return 0;
  return reference_history_[size_t(sample)%reference_history_.size()];
}
float OrdinaryBargeCapture::MicrophoneAt(int64_t sample) const {
  if(sample<0 || uint64_t(sample)>=history_samples_ || history_samples_-uint64_t(sample)>microphone_history_.size())return 0;
  return microphone_history_[size_t(sample)%microphone_history_.size()];
}
bool OrdinaryBargeCapture::FindEchoPath(int64_t end,int* delay,double* coherence) const {
  // Forty milliseconds, decimated by eight for a bounded search at every
  // unit lag (decimating lags themselves would miss independent-noise echo).
  // All indices refer to actual history, never a configured acoustic delay.
  if(end<640)return false;
  auto correlation=[&](int lag) {
    double mm=1,rr=1,mr=0;
    for(int j=0;j<640;j+=8) {
      const double m=MicrophoneAt(end-j),r=ReferenceAt(end-j-lag);
      mm+=m*m;rr+=r*r;mr+=m*r;
    }
    if(rr<80*72.0*72.0)return 0.0;
    return std::abs(mr)/std::sqrt(mm*rr);
  };
  if(path_bound_) {
    const double existing=correlation(echo_delay_samples_);
    if(existing>=0.85) {*delay=echo_delay_samples_;*coherence=existing;return true;}
  }
  int best=-32;double score=0;
  for(int lag=-32;lag<=1440;++lag) {const double value=correlation(lag);if(value>score){score=value;best=lag;}}
  *delay=best;*coherence=score;return score>=0.65;
}
bool OrdinaryBargeCapture::ResidualContainsEcho(int64_t end) const {
  // Same 40ms / eight-sample grid as acquisition, but on the frozen predictor
  // error rather than raw microphone. Also cover 100ms and the supported FIR
  // tail so a newly delayed echo cannot masquerade as doubletalk. At most one
  // 1760 * 80 search per five frames, and only before near-end accumulation.
  // Signed reference detects linear leakage. A centered absolute-reference
  // basis also detects same-source even distortion, including after a slow
  // DC removal. It uses this same traversal, without another lag search or
  // changing the real VAD, level, streak or capture-continuity requirements.
  double mm=1,m_sum=0;
  for(int j=0;j<640;j+=8) {
    const double m=predictor_residual_history_[size_t(end-j)%predictor_residual_history_.size()];
    mm+=m*m;m_sum+=m;
  }
  const double centered_mm=std::max(0.0,mm-1-m_sum*m_sum/80);
  for(int lag=-32;lag<=1727;++lag) {
    double rr=1,mr=0,abs_r_sum=0,m_abs_r_sum=0;
    for(int j=0;j<640;j+=8) {
      const double m=predictor_residual_history_[size_t(end-j)%predictor_residual_history_.size()];
      const double r=ReferenceAt(end-j-lag);
      rr+=r*r;mr+=m*r;
      const double abs_r=std::abs(r);
      abs_r_sum+=abs_r;m_abs_r_sum+=m*abs_r;
    }
    if(rr>=80*72.0*72.0 && std::abs(mr)>=0.65*std::sqrt(mm*rr))return true;
    const double centered_abs_rr=std::max(0.0,rr-1-abs_r_sum*abs_r_sum/80);
    const double centered_m_abs_r=m_abs_r_sum-m_sum*abs_r_sum/80;
    // A flat absolute reference is not usable evidence; reuse the existing
    // reference level floor for the centered basis, not a lower threshold.
    if(centered_mm>0 && centered_abs_rr>=80*72.0*72.0 &&
        std::abs(centered_m_abs_r)>=0.65*std::sqrt(centered_mm*centered_abs_rr))return true;
  }
  return false;
}
void OrdinaryBargeCapture::ClearPreroll() { preroll_.clear(); voice_streak_=0; }
std::vector<CapturedVoiceFrame> OrdinaryBargeCapture::TakePreroll() {
  std::vector<CapturedVoiceFrame> out(preroll_.begin(),preroll_.end());
  ClearPreroll(); return out;
}
BargeCaptureResult OrdinaryBargeCapture::Push(const shadow::Context& context,
    const CaptureStamp& stamp, const std::vector<uint8_t>& pcm, bool can_confirm,
    bool playback_requires_reference, bool actual_input_needed) {
  BargeCaptureResult out;
  auto fail=[&]() { ready_=false;faulted_=true;fault_generation_=stamp.generation;
    input_.ClearContinuity();ClearPreroll();out.frames.clear();out.fault=true;return out; };
  if(!context.valid() || !stamp.generation || context.capture!=stamp.generation ||
      !stamp.continuous || !stamp.first_read_ns || !stamp.delivered_ns ||
      stamp.first_read_ns>stamp.delivered_ns || hint_<0 || hint_>500) return fail();
  if(faulted_ && stamp.generation<=fault_generation_)return fail();
  if(context!=context_) {
    if(!input_.Begin(context))return fail();
    context_=context;ClearPreroll();
    echo_weights_={};reference_history_={};microphone_history_={};predictor_residual_history_={};
    history_samples_=0;path_bound_=fit_confident_=false;
    fit_good_frames_=weak_reference_frames_=frozen_fit_frames_=0;
    last_residual_check_sample_=0;residual_echo_clear_=false;
    echo_processing_enabled_=false;echo_tail_frames_=0;
    ready_=aec_.kind()==shadow::BackendKind::kRealBackend &&
      vad_.kind()==shadow::BackendKind::kRealBackend && aec_.Reset() && vad_.Reset();
    if(!ready_)return fail();
    faulted_=false;
  }
  if(!ready_)return fail();
  shadow::LocalChunk chunk{context,stamp.first_sample,stamp.first_sample,
                           stamp.delivered_ns,pcm.data(),pcm.size(),true};
  const auto batch=input_.Push(chunk);
  if(batch.fault!=shadow::InputFault::kNone)return fail();
  for(size_t i=0;i<batch.count;++i) {
    const auto& paired=batch.frames[i];
    const auto cleaned=aec_.Process(paired,hint_);
    if(!cleaned.health.healthy())return fail();
    const auto voice=vad_.Analyze(cleaned.cleaned);
    if(!voice.initialized || voice.process_status || voice.voice==shadow::Voice::kUnknown)return fail();
    double frame_reference_energy=0;
    for(const auto sample:paired.reference)frame_reference_energy+=double(sample)*sample;
    const bool reference_tail=echo_tail_frames_>0;
    if(playback_requires_reference)echo_tail_frames_=20;
    else if(echo_tail_frames_)--echo_tail_frames_;
    // Fresh input still needs the original voiced/quiet-reference classifier
    // after playback stops. Input eligibility does not reopen confirmation.
    const bool process_echo=can_confirm || playback_requires_reference ||
      reference_tail || actual_input_needed;
    if(process_echo!=echo_processing_enabled_) {
      // Never carry an idle/unobserved predictor into a new playback window.
      // This does not reset the real APM/VAD or the physical continuity latch.
      ClearPreroll();echo_weights_={};reference_history_={};microphone_history_={};
      predictor_residual_history_={};history_samples_=0;echo_delay_samples_=0;
      path_bound_=fit_confident_=false;
      fit_good_frames_=weak_reference_frames_=frozen_fit_frames_=0;
      last_residual_check_sample_=0;residual_echo_clear_=false;
      echo_processing_enabled_=process_echo;
    }
    if(!process_echo) {
      // The input adapter, APM and VAD above still attest this delivery's
      // health. No inactive PCM can train/confirm an echo path or near end.
      out.reference_active=frame_reference_energy>=shadow::kSamplesPerFrame*72.0*72.0;
      out.echo_path_confident=false;out.echo_delay_samples=0;
      CapturedVoiceFrame frame{context,paired.stamp.mic_sample,stamp.first_read_ns,
                              paired.stamp.delivered_ns,cleaned.cleaned,false};
      out.frames.push_back(frame);preroll_.push_back(frame);
      while(preroll_.size()>20)preroll_.pop_front();
      continue;
    }
    const uint64_t frame_start=history_samples_;
    for(size_t j=0;j<shadow::kSamplesPerFrame;++j) {
      reference_history_[history_samples_%reference_history_.size()]=paired.reference[j];
      microphone_history_[history_samples_%microphone_history_.size()]=paired.mic[j];++history_samples_;
    }
    int selected_delay=echo_delay_samples_;double path_coherence=0;
    const bool trusted_at_start=fit_confident_;
    const bool coherent=!trusted_at_start && frame_reference_energy>=shadow::kSamplesPerFrame*72.0*72.0 &&
      FindEchoPath(int64_t(history_samples_)-33,&selected_delay,&path_coherence);
    if(coherent && (!path_bound_ || selected_delay!=echo_delay_samples_)) {
      // Path changes revoke confidence immediately. A new predictor must
      // establish its own fit; no old-room confidence is carried forward.
      echo_delay_samples_=selected_delay;echo_weights_={};path_bound_=true;
      fit_confident_=false;fit_good_frames_=frozen_fit_frames_=0;voice_streak_=0;
      residual_echo_clear_=false;last_residual_check_sample_=0;
    }
    double energy=0,ref_energy=0,unexplained=0,mic_energy=0;
    bool saturated=false;
    for(size_t j=0;j<shadow::kSamplesPerFrame;++j) {
      const double m=cleaned.cleaned[j],r=paired.reference[j];
      saturated=saturated || std::abs(int(paired.mic[j]))>=32760 || std::abs(int(paired.reference[j]))>=32760;
      energy+=m*m;ref_energy+=r*r;
      const int64_t sample=int64_t(frame_start+j)-32;
      const double delayed_mic=MicrophoneAt(sample);mic_energy+=delayed_mic*delayed_mic;
      double predicted=0,normalization=1;
      for(size_t tap=0;tap<echo_weights_.size();++tap) {
        const double x=ReferenceAt(sample-echo_delay_samples_-int64_t(tap));
        predicted+=echo_weights_[tap]*x;normalization+=x*x;
      }
      const double error=delayed_mic-predicted;unexplained+=error*error;
      if(sample>=0)predictor_residual_history_[size_t(sample)%predictor_residual_history_.size()]=float(error);
      // Adapt only while acquiring. Speech must never train the trusted echo
      // predictor, including frames where raw mic/ref coherence has dropped.
      if(!trusted_at_start && path_bound_ && normalization>128*72.0*72.0) {
        const double step=0.75*error/normalization;
        for(size_t tap=0;tap<echo_weights_.size();++tap) {
          echo_weights_[tap]+=float(step*ReferenceAt(sample-echo_delay_samples_-int64_t(tap)));
        }
      }
    }
    const bool active_reference=ref_energy>=shadow::kSamplesPerFrame*72.0*72.0;
    // A quiet-reference tail also lasts beyond the supported maximum lag.
    // Previous reference/room tail is never immediately labelled near-end.
    weak_reference_frames_=active_reference ? 0 : weak_reference_frames_+1;
    if(trusted_at_start && frozen_fit_frames_<4)++frozen_fit_frames_;
    if(saturated) {
      path_bound_=fit_confident_=false;echo_weights_={};fit_good_frames_=frozen_fit_frames_=0;
      residual_echo_clear_=false;voice_streak_=0;
    }else if(active_reference && !trusted_at_start) {
      if(!coherent) {fit_confident_=false;fit_good_frames_=0;}
      else if(unexplained<std::min(std::max(1.0,mic_energy)*0.02,
                                  shadow::kSamplesPerFrame*36.0*36.0)) {
        if(++fit_good_frames_>=5)fit_confident_=true;
      }else fit_good_frames_=0;
    }else fit_good_frames_=0;
    const bool possible_voice=voice.voice==shadow::Voice::kVoice &&
      energy>=shadow::kSamplesPerFrame*72.0*72.0 &&
      unexplained>=shadow::kSamplesPerFrame*72.0*72.0;
    if(active_reference && fit_confident_ && possible_voice && frozen_fit_frames_>=4 &&
        (!residual_echo_clear_ || history_samples_-last_residual_check_sample_>=5*shadow::kSamplesPerFrame)) {
      last_residual_check_sample_=history_samples_;
      residual_echo_clear_=!ResidualContainsEcho(int64_t(history_samples_)-33);
      if(!residual_echo_clear_) {
        // Correlated predictor error is actual unmodelled echo evidence, as
        // distinct from the raw correlation loss caused by near-end speech.
        path_bound_=fit_confident_=false;echo_weights_={};
        fit_good_frames_=frozen_fit_frames_=0;voice_streak_=0;
      }
    }
    // Actual independent APM VAD and its residual level floor are mandatory.
    // The frozen predictor and leakage veto only validate the reference path.
    const bool voice_frame=!saturated && voice.voice==shadow::Voice::kVoice &&
      energy>=shadow::kSamplesPerFrame*72.0*72.0 &&
      (active_reference ? (fit_confident_ && residual_echo_clear_ && frozen_fit_frames_>=4 &&
        unexplained>=shadow::kSamplesPerFrame*72.0*72.0) :
        (!playback_requires_reference && weak_reference_frames_>=20));
    out.reference_active=active_reference;out.echo_path_confident=fit_confident_ && active_reference;
    out.echo_delay_samples=echo_delay_samples_;
    CapturedVoiceFrame frame{context,paired.stamp.mic_sample,stamp.first_read_ns,
                            paired.stamp.delivered_ns,cleaned.cleaned,voice_frame};
    out.frames.push_back(frame);preroll_.push_back(frame);
    while(preroll_.size()>20)preroll_.pop_front();
    voice_streak_=voice_frame ? voice_streak_+1 : 0;
    if(can_confirm && voice_streak_>=6)out.confirmed=true;
  }
  return out;
}
}  // namespace xiaoai_plus::audio
