#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
namespace xiaoai_plus::audio {
enum class SessionCue { kNone, kWake, kExit };
// 160 ms at the existing 24 kHz mono S16LE output, with a 12 ms soft edge.
inline std::vector<uint8_t> MakeSessionCue(SessionCue cue,float gain) {
  if(cue==SessionCue::kNone)return {};
  constexpr int samples=3840,edge=288;
  constexpr double pi=3.14159265358979323846;
  std::vector<uint8_t> pcm(samples*2);
  double phase=0;
  for(int i=0;i<samples;++i) {
    const double t=double(i)/samples;
    const double hz=cue==SessionCue::kWake?660+220*t:660-220*t;
    const double envelope=std::min(1.0,std::min(double(i)/edge,double(samples-1-i)/edge));
    const int16_t value=static_cast<int16_t>(std::sin(phase)*32767*0.12*gain*envelope);
    phase+=2*pi*hz/24000;
    pcm[2*i]=static_cast<uint16_t>(value)&255;pcm[2*i+1]=static_cast<uint16_t>(value)>>8;
  }
  return pcm;
}
} // namespace xiaoai_plus::audio
