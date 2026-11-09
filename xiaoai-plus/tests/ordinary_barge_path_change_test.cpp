#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include "audio/ordinary_barge_capture.hpp"
#include "audio/shadow_webrtc.hpp"
using namespace xiaoai_plus::audio;

// Synthetic path transitions complement, and never replace, the immutable
// independent real-human 21-combination probe. No audio is written or played.
void Transition(int next_delay, double next_gain, double secondary,
                bool weak_reference, bool lost_reference) {
  constexpr size_t frames=360, change_frame=180;
  uint32_t seed=749;
  std::vector<int16_t> reference(frames*160);
  for(size_t i=0;i<reference.size();++i) {
    seed=seed*1664525+1013904223;
    reference[i]=i<3200 ? 0 : int((seed>>16)&8191)-4096;
  }
  auto read=[&](int sample) {return sample<0 ? 0 : int(reference[size_t(sample)]);};
  shadow::PairedWebrtcAec aec;shadow::WebrtcVoiceDetector vad;
  OrdinaryBargeCapture capture(aec,vad,90);
  shadow::Context context{1,1,1,1,1,1};
  size_t confirms=0,confident_before=0,clips=0,faults=0,pairs_at_fault=0;
  for(size_t frame=0;frame<frames;++frame) {
    const bool changed=frame>=change_frame;
    std::vector<uint8_t> bytes(1280);
    for(size_t j=0;j<160;++j) {
      const int sample=int(frame*160+j);
      const int delay=changed ? next_delay : 0;
      const double gain=changed ? next_gain : .5;
      const double mic=gain*read(sample-delay)+(changed ? secondary*read(sample-delay-7) : 0);
      clips+=std::abs(mic)>=32760;
      const int16_t near=std::max(-32768.0,std::min(32767.0,mic));
      const int16_t far=changed && lost_reference ? 0 :
        int16_t(reference[size_t(sample)]*(changed && weak_reference ? .01 : 1));
      for(int c=0;c<4;++c) {const int16_t v=c==3 ? far : near;std::memcpy(bytes.data()+j*8+c*2,&v,2);}
    }
    const uint64_t delivered=1000000000ULL+frame*10000000ULL;
    const auto result=capture.Push(context,{1,frame*160,delivered-10000000ULL,delivered,true},bytes,true,true);
    if(result.fault) {
      assert(result.frames.empty() && !result.confirmed);
      // Unsupported delay / clipping / unavailable reference are rejection
      // cases. A real APM health failure must stay latched, never be hidden.
      assert(next_delay>1440 || next_gain>=16 || weak_reference || lost_reference);
      if(!faults) {
        pairs_at_fault=aec.successful_pairs();
        std::cout<<"ACTUAL_BACKEND_FAILURE delay="<<next_delay<<" frame="<<frame
          <<" capture_api_status="<<aec.last_step().capture_api_status
          <<" capture_health_status="<<aec.last_step().health.capture_status
          <<" nonfinite="<<aec.last_step().nonfinite_residual_samples<<std::endl;
      }
      ++faults;assert(aec.successful_pairs()==pairs_at_fault);
      continue;
    }
    assert(faults==0 && result.frames.size()==1);
    confirms+=result.confirmed;
    if(!changed && result.echo_path_confident)++confident_before;
  }
  std::cout<<"TRUSTED_PATH_TRANSITION delay_samples="<<next_delay<<" gain="<<next_gain
    <<" FIR_secondary="<<secondary<<" weak_reference="<<weak_reference
    <<" lost_reference="<<lost_reference<<" confident_before="<<confident_before
    <<" confirmed="<<confirms<<" fault_frames="<<faults<<" clipped="<<clips<<std::endl;
  assert(confident_before>100 && confirms==0);
}
int main() {
  for(int delay:{0,80,160,320,800,1440,1600})Transition(delay,.5,0,false,false);
  Transition(16,.5,.3,false,false);
  Transition(0,2,0,false,false);
  Transition(0,16,0,false,false);
  Transition(0,.5,0,true,false);
  Transition(0,.5,0,false,true);
  std::cout<<"PASS trusted_path_changes_gain_FIR_clip_reference_loss no_pure_echo_confirmation\n";
}
