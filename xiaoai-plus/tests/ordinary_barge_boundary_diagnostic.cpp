#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include "audio/ordinary_barge_capture.hpp"
#include "audio/shadow_webrtc.hpp"
using namespace xiaoai_plus::audio;
std::vector<int16_t> Reference(size_t n) {
  uint32_t seed=749;std::vector<int16_t> reference(n+200);
  for(auto& v:reference) {seed=seed*1664525+1013904223;v=int((seed>>16)&8191)-4096;}
  for(size_t i=0;i<3200 && i<reference.size();++i)reference[i]=0;
  return reference;
}
int16_t Read(const std::vector<int16_t>& x,int index) {return index<0 || size_t(index)>=x.size() ? 0 : x[index];}
size_t Run(const std::vector<int16_t>& speech,int delay,double gain,double secondary,double speech_gain,
           const char* label,const std::vector<int16_t>* human_reference=nullptr,double reference_scale=1.0) {
  const size_t total=human_reference ? 6400+human_reference->size() : 32000+speech.size();auto reference=Reference(total);
  if(human_reference)for(size_t i=0;i<reference.size();++i)reference[i]=i>=3200 && i-3200<human_reference->size() ? (*human_reference)[i-3200] : 0;
  for(auto& v:reference)v=int16_t(v*reference_scale);
  shadow::PairedWebrtcAec aec;shadow::WebrtcVoiceDetector vad;OrdinaryBargeCapture capture(aec,vad,90);
  shadow::Context context{1,1,1,1,1,1};size_t confirms=0,unmixed_confirms=0,first_confirm=0,confident_frames=0;
  for(size_t frame=0;frame*160+160<=total;++frame) {
    std::vector<uint8_t> bytes(1280);
    for(size_t i=0;i<160;++i) {
      const int sample=int(frame*160+i);const bool active=sample>=3200;
      const int16_t far=active ? reference[size_t(sample)] : 0;
      double mic=active ? gain*Read(reference,sample-delay)+secondary*Read(reference,sample-delay-7):0;
      if(sample>=32000 && size_t(sample-32000)<speech.size())mic+=speech_gain*speech[size_t(sample-32000)];
      const int16_t near=std::max(-32768.0,std::min(32767.0,mic));
      for(int c=0;c<4;++c) {const auto v=c==3?far:near;std::memcpy(bytes.data()+i*8+c*2,&v,2);}
    }
    const uint64_t delivered=1000000000ULL+frame*10000000ULL;
    auto result=capture.Push(context,CaptureStamp{1,frame*160,delivered-10000000ULL,delivered,true},bytes,true,true);
    assert(!result.fault && result.frames.size()==1);
    if(result.echo_path_confident)++confident_frames;
    if(result.confirmed) {if(!confirms)first_confirm=frame*160;++confirms;if(frame*160<32000)++unmixed_confirms;}
  }
  std::cout<<label<<" delay_samples="<<delay<<" gain="<<gain<<" FIR_secondary="<<secondary
    <<" speech_gain="<<speech_gain<<" confirmed_frames="<<confirms<<" pure_echo_frames="<<unmixed_confirms<<" first_confirm_sample="<<first_confirm<<" confident_frames="<<confident_frames<<std::endl;
  return confirms;
}
int main(int argc,char** argv) {
  assert(argc==2);std::ifstream input(argv[1],std::ios::binary);assert(input);
  // P2d real-human guided word window seq1, immutable original 4ch S32LE.
  // Historical source speaker age/ref/room unknown. Only ch0 is consumed in
  // RAM and mixed with known synthetic echo; no PCM output, playback or upload.
  constexpr size_t begin=274560,count=80160;input.seekg(begin*16);
  std::vector<int16_t> speech;for(size_t i=0;i<count;++i) {
    int32_t four[4]{};input.read(reinterpret_cast<char*>(four),16);assert(input);
    speech.push_back(static_cast<int16_t>(four[0]>>16));
  }
  size_t echo_failures=0,human_misses=0;
  for(int delay:{0,80,160,320,800,1440}) {
    if(Run({},delay,1,0,0,"diagnostic_synthetic_echo_only")!=0)++echo_failures;
  }
  for(int delay:{0,160,1440})if(Run({},delay,.5,0,0,"historical_human_reference_pure_echo",&speech)!=0)++echo_failures;
  if(Run({},16,.5,.3,0,"historical_human_reference_FIR_pure_echo",&speech)!=0)++echo_failures;
  if(Run({},1600,1,0,0,"unsupported_100ms_echo_failclosed")!=0)++echo_failures;
  if(Run({},0,100,0,0,"weak_reference_amplified_echo_failclosed",nullptr,.01)!=0)++echo_failures;
  if(Run({},0,16,0,0,"diagnostic_nonlinear_s16_clipped_echo_only")!=0)++echo_failures;
  if(Run({},16,.5,.3,0,"diagnostic_synthetic_FIR_echo_only")!=0)++echo_failures;
  if(!Run(speech,0,.5,0,1,"historical_human_plus_synthetic_echo"))++human_misses;
  if(!Run(speech,16,.5,.3,1,"historical_human_plus_synthetic_FIR_echo"))++human_misses;
  if(!Run(speech,0,.5,0,.5,"attenuated_historical_human_plus_echo"))++human_misses;
  std::cout<<"BOUNDARY_RESULT echo_cases_failed="<<echo_failures<<" historical_human_cases_missed="<<human_misses<<" room_child_UNKNOWN\n";
  return echo_failures || human_misses ? 1 : 0;
}
