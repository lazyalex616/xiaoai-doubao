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
using namespace std::chrono_literals;
using P=xiaoai_plus::audio::AplayPlayer;
int main() {
  char temp[]="/tmp/xiaoai-barge-player-XXXXXX"; assert(mkdtemp(temp));
  const std::string fake=std::string(temp)+"/aplay";
  { std::ofstream f(fake); f << "#!/usr/bin/env python3\nimport os,time\n"
      "mode=os.environ['BARGE_FAKE_MODE']\n"
      "if mode=='blocked': time.sleep(10)\n"
      "while True:\n data=os.read(0,4096)\n if not data: break\n"
      " time.sleep(0.01 if mode=='progress' else 0.10)\n"; }
  assert(chmod(fake.c_str(),0755)==0);
  const std::string path=std::string(temp)+":"+(getenv("PATH")?getenv("PATH"):"/usr/bin:/bin");
  assert(setenv("PATH",path.c_str(),1)==0);
  for(const auto* mode:{"progress","slow","blocked"}) {
    assert(setenv("BARGE_FAKE_MODE",mode,1)==0);
    xiaoai_plus::config::Audio cfg; cfg.sample_rate=24000;cfg.channels=1;cfg.bits_per_sample=16;
    cfg.output_device="fake";
    P p(cfg,4); std::mutex mu;std::condition_variable cv;int progress=0,legacy=0,failures=0,cancels=0,written=0;
    uint64_t last_generation=0;
    p.SetOnWriteProgress([&]{std::lock_guard<std::mutex> l(mu);++progress;cv.notify_all();});
    p.SetOnChunkProcessed([&](bool ok){std::lock_guard<std::mutex> l(mu);++legacy;if(!ok)++failures;cv.notify_all();});
    p.SetOnScopedChunkProcessed([&](uint64_t g,P::ChunkResult r){std::lock_guard<std::mutex> l(mu);last_generation=g;if(r==P::ChunkResult::kCanceled)++cancels;if(r==P::ChunkResult::kWritten)++written;cv.notify_all();});
    assert(p.Start()); const auto old=p.Generation();
    assert(p.Play(std::vector<uint8_t>(2*1024*1024,0x22),old));
    {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return progress>0;}));}
    std::this_thread::sleep_for(120ms); // Writer is mid-chunk, not just queued.
    assert(setenv("BARGE_FAKE_MODE","progress",1)==0);
    const auto start=std::chrono::steady_clock::now();
    bool interrupted=false;double ms=0;
    std::thread interrupter([&]{interrupted=p.Interrupt(old);ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();});
    for(int i=0;i<1000 && p.Generation()==old;++i) std::this_thread::sleep_for(1ms);
    assert(p.Generation()==old+1);
    // Queue new playback while the old interrupt is still stopping its pipe.
    const auto next=p.Generation();assert(p.Play(std::vector<uint8_t>(640,0),next));
    interrupter.join();assert(interrupted);
    assert(ms<600); assert(p.Generation()==old+1);
    {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,1s,[&]{return cancels==1;}));assert(failures==0 && cancels==1);}
    assert(!p.Interrupt(old)); assert(!p.Play(std::vector<uint8_t>(640,0),old));
    // This new generation must survive a repeated old interrupt.
    assert(!p.Interrupt(old));
    // Scoped and legacy notifications are separate steps; wait for both before
    // checking the final count, rather than racing the scoped notification.
    {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return written==1 && legacy>=1;}));assert(legacy==1 && failures==0 && last_generation==next);}
    p.Close();std::cout<<"PASS mode="<<mode<<" interrupt_ms="<<ms<<" canceled=1 failures=0 concurrent_new_written=1\n";
  }
  assert(unlink(fake.c_str())==0);assert(rmdir(temp)==0);
}
