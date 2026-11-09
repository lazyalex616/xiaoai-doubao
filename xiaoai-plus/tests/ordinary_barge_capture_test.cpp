#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include "audio/ordinary_barge_capture.hpp"
#include "audio/shadow_webrtc.hpp"
using namespace xiaoai_plus::audio;
int main() {
  shadow::PairedWebrtcAec aec;shadow::WebrtcVoiceDetector vad;
  OrdinaryBargeCapture pipeline(aec,vad,90);
  shadow::Context origin{1,1,1,1,1,1};
  uint32_t noise=1234567;size_t echo_confirm=0,mixture_confirm=0,voiced=0,frames=0;
  uint64_t first_mixture=0;
  for(size_t frame=0;frame<240;++frame) {
    std::vector<uint8_t> four(160*8);
    for(size_t i=0;i<160;++i) {
      noise=noise*1664525+1013904223;const int far=int((noise>>16)&8191)-4096;
      const double phase=double(frame*160+i)/16000;
      const int16_t reference=frame<20 ? 0 : far;
      const int16_t microphone=frame<20 ? 0 : far/2+(frame>=160 ? 5000*std::sin(2*3.141592653589793*220*phase):0);
      for(int c=0;c<4;++c) {const int16_t v=c==3?reference:microphone;std::memcpy(four.data()+i*8+c*2,&v,2);}
    }
    const uint64_t delivered=1000000000ULL+frame*10000000ULL;
    CaptureStamp stamp{1,frame*160,delivered-10000000ULL,delivered,true};
    const auto result=pipeline.Push(origin,stamp,four,true);assert(!result.fault && result.frames.size()==1);
    assert(result.frames[0].origin==origin && result.frames[0].first_sample==frame*160);
    assert(result.frames[0].first_read_ns==stamp.first_read_ns && result.frames[0].delivered_ns==stamp.delivered_ns);
    frames+=result.frames.size();if(result.frames[0].voiced)++voiced;
    if(result.confirmed) {if(frame<160)++echo_confirm;else {++mixture_confirm;if(!first_mixture)first_mixture=frame*160;}}
  }
  auto preroll=pipeline.TakePreroll();assert(preroll.size()==20);assert(pipeline.TakePreroll().empty());
  for(size_t i=1;i<preroll.size();++i)assert(preroll[i].first_sample==preroll[i-1].first_sample+160);
  std::cout<<"ACTUAL_APM frames="<<frames<<" residual_vad_plus_veto_frames="<<voiced<<" echo_confirm="<<echo_confirm
    <<" mixture_confirm="<<mixture_confirm<<" first_mixture_sample="<<first_mixture<<std::endl;
  assert(echo_confirm==0 && mixture_confirm>0);
  std::vector<uint8_t> silence(1280);
  assert(!pipeline.Push(origin,CaptureStamp{1,240*160,3740000000ULL,3750000000ULL,true},silence,true).fault);
  const auto fault=pipeline.Push(origin,CaptureStamp{1,242*160,3750000000ULL,3760000000ULL,true},silence,true);
  assert(fault.fault && pipeline.TakePreroll().empty()); // A gap cannot be retried as fresh data.
  auto changed=origin;changed.session=2;const auto resets=aec.successful_pairs();
  assert(pipeline.Push(changed,CaptureStamp{1,240*160,3400000000ULL,3410000000ULL,true},silence,true).fault);
  assert(aec.successful_pairs()==resets);
  changed.capture=2;
  assert(!pipeline.Push(changed,CaptureStamp{2,0,4000000000ULL,4010000000ULL,true},silence,false).fault);
  assert(pipeline.TakePreroll().size()==1);
  std::cout<<"PASS single_transfer_original_provenance real_AEC_VAD_scripted_synthetic_only fault_requires_new_producer_generation\n";
}
