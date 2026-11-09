#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "audio/player.hpp"
#include "audio/session_cue.hpp"
using P=xiaoai_plus::audio::AplayPlayer;
using namespace std::chrono_literals;
int main(){
 char d[]="/tmp/xiaoai-review-reentrant-XXXXXX";assert(mkdtemp(d));
 std::string fake=std::string(d)+"/aplay";
 {std::ofstream f(fake);f<<"#!/usr/bin/env python3\nimport os\nwhile os.read(0,4096): pass\n";}assert(chmod(fake.c_str(),0755)==0);
 std::string path=std::string(d)+":"+(getenv("PATH")?getenv("PATH"):"/usr/bin:/bin");assert(setenv("PATH",path.c_str(),1)==0);
 xiaoai_plus::config::Audio cfg;cfg.sample_rate=24000;cfg.channels=1;cfg.bits_per_sample=16;cfg.output_device="fake";
 P p(cfg,4);std::mutex mu;std::condition_variable cv;int legacy=0,scoped=0;uint64_t old=0;
 p.SetOnChunkProcessed([&](bool ok){assert(ok);std::lock_guard<std::mutex> l(mu);++legacy;cv.notify_all();});
 p.SetOnScopedChunkProcessed([&](uint64_t g,P::ChunkResult result){assert(result==P::ChunkResult::kWritten);if(g==old){assert(p.Interrupt(g));assert(p.Play(std::vector<uint8_t>(640,0),p.Generation()));}std::lock_guard<std::mutex> l(mu);++scoped;cv.notify_all();});
 assert(p.Start());
 const auto wake=xiaoai_plus::audio::MakeSessionCue(xiaoai_plus::audio::SessionCue::kWake,0.75f);
 const auto exit=xiaoai_plus::audio::MakeSessionCue(xiaoai_plus::audio::SessionCue::kExit,0.75f);
 assert(wake.size()==7680 && exit.size()==7680 && wake!=exit);
 assert(xiaoai_plus::audio::MakeSessionCue(xiaoai_plus::audio::SessionCue::kNone,0.75f).empty());
 assert(wake.front()==0 && wake.back()==0);
 for(const auto& cue:{wake,exit}) {
   for(size_t i=0;i<cue.size();i+=2) {
     const int16_t value=static_cast<int16_t>(cue[i]|(uint16_t(cue[i+1])<<8));
     assert(std::abs(int(value))<=2950); // Soft tone includes the normal 0.75 playback gain.
   }
   assert(p.Play(cue,p.Generation(),false));
   assert(p.Drain(p.Generation(),500ms));
   assert(legacy==0 && scoped==0); // Each local cue drains once, without cloud completion effects.
 }
 old=p.Generation();assert(p.Play(std::vector<uint8_t>(640,0),old));
 {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,3s,[&]{return scoped==2&&legacy>=1;}));}
 p.Close();std::cout<<"scoped_completed="<<scoped<<" legacy_completed="<<legacy<<" expected_legacy=1 after_old_generation_invalidated_inside_scoped_callback\n";
 assert(unlink(fake.c_str())==0);assert(rmdir(d)==0);
 return legacy==1?0:2;
}
