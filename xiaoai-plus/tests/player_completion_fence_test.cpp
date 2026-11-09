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
using P=xiaoai_plus::audio::AplayPlayer;
using namespace std::chrono_literals;
int main() {
  char dir[]="/tmp/xiaoai-completion-fence-XXXXXX";assert(mkdtemp(dir));
  const std::string fake=std::string(dir)+"/aplay";
  {std::ofstream f(fake);f<<"#!/usr/bin/env python3\nimport os\nwhile os.read(0,4096): pass\n";}
  assert(chmod(fake.c_str(),0755)==0);
  const std::string path=std::string(dir)+":"+(getenv("PATH")?getenv("PATH"):"/usr/bin:/bin");
  assert(setenv("PATH",path.c_str(),1)==0);
  xiaoai_plus::config::Audio cfg;cfg.sample_rate=24000;cfg.channels=1;cfg.bits_per_sample=16;cfg.output_device="fake";
  for(const bool shutdown:{false,true}) {
    P p(cfg,4);std::mutex mu;std::condition_variable cv;bool entered=false,release=false;
    std::atomic<bool> cancel_started{false},cancel_returned{false};int scoped=0,legacy=0;
    uint64_t old=0;
    p.SetOnScopedChunkProcessed([&](uint64_t g,P::ChunkResult result){
      assert(g==old && result==P::ChunkResult::kWritten);
      std::unique_lock<std::mutex> lock(mu);++scoped;entered=true;cv.notify_all();
      assert(cv.wait_for(lock,2s,[&]{return release;}));
    });
    p.SetOnChunkProcessed([&](bool ok){
      assert(ok && p.Generation()==old);
      std::lock_guard<std::mutex> lock(mu);++legacy;cv.notify_all();
    });
    assert(p.Start());old=p.Generation();assert(p.Play(std::vector<uint8_t>(640,0),old));
    {std::unique_lock<std::mutex> lock(mu);assert(cv.wait_for(lock,2s,[&]{return entered;}));}
    std::thread canceler([&]{cancel_started.store(true);if(shutdown)p.Close();else assert(p.Interrupt(old));cancel_returned.store(true);});
    for(int i=0;i<1000 && !cancel_started.load();++i)std::this_thread::sleep_for(1ms);
    assert(cancel_started.load());std::this_thread::sleep_for(30ms);assert(!cancel_returned.load());
    {std::lock_guard<std::mutex> lock(mu);release=true;}cv.notify_all();
    canceler.join();assert(cancel_returned.load());
    {std::lock_guard<std::mutex> lock(mu);assert(scoped==1 && legacy==1);}
    p.Close();std::cout<<"PASS concurrent_"<<(shutdown?"close":"interrupt")<<"_publication_after_callback_fence legacy_before_invalidation=1 no_deadlock\n";
  }
  {
    P p(cfg,4);std::mutex mu;std::condition_variable cv;int completed=0;
    p.SetOnChunkProcessed([&](bool ok){assert(ok);std::lock_guard<std::mutex> lock(mu);++completed;cv.notify_all();});
    assert(p.Start());assert(p.Play(std::vector<uint8_t>(640,0)));assert(p.Play(std::vector<uint8_t>(640,0)));
    {std::unique_lock<std::mutex> lock(mu);assert(cv.wait_for(lock,2s,[&]{return completed==2;}));}
    p.Close();std::cout<<"PASS legacy_only_no_scoped_default_path completed=2\n";
  }
  assert(unlink(fake.c_str())==0);assert(rmdir(dir)==0);
}
