#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "audio/player.hpp"
using P=xiaoai_plus::audio::AplayPlayer;
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
int main() {
  char dir[]="/tmp/xiaoai-audible-XXXXXX";assert(mkdtemp(dir));
  const std::string fake=std::string(dir)+"/aplay";
  {std::ofstream f(fake);f<<"#!/usr/bin/env python3\nimport os,time\nnext_read=time.monotonic()\nwhile os.read(0,720):\n next_read+=.015\n time.sleep(max(0,next_read-time.monotonic()))\n";}
  assert(chmod(fake.c_str(),0755)==0);
  const auto path=std::string(dir)+":"+getenv("PATH");assert(setenv("PATH",path.c_str(),1)==0);
  xiaoai_plus::config::Audio cfg;cfg.sample_rate=24000;cfg.channels=1;cfg.bits_per_sample=16;cfg.output_device="fake";
  for(int mode=0;mode<3;++mode) {
    P p(cfg,4);std::mutex mu;std::condition_variable cv;
    Clock::time_point first{},last_write{},callback{};int callbacks=0;size_t progress=0;
    p.SetOnWriteProgress([&]{std::lock_guard<std::mutex> l(mu);last_write=Clock::now();if(first==Clock::time_point{})first=last_write;++progress;cv.notify_all();});
    p.SetOnChunkProcessed([&](bool ok){assert(ok);std::lock_guard<std::mutex> l(mu);if(!callbacks)callback=Clock::now();++callbacks;cv.notify_all();});
    assert(p.Start());assert(p.Play(std::vector<uint8_t>(48000)));
    // Wait until the final pipe write: writer now enters its audible wait.
    {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,3s,[&]{return progress>=10;}));}
    std::this_thread::sleep_for(30ms);
    if(mode==0) {
      std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,3s,[&]{return callbacks==1;}));
      const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(callback-first).count();
      const auto tail_ms=200+1000*cfg.buffer_size/cfg.sample_rate;
      assert(ms>=900 && ms<=1000+tail_ms+400);std::cout<<"PASS audible callback_ms="<<ms<<" tail_ms="<<tail_ms<<std::endl;
    }else if(mode==1) {
      const auto began=Clock::now();assert(p.Play(std::vector<uint8_t>(4800)));
      std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,100ms,[&]{return callbacks>=1 && progress>10;}));
      assert(callback-began<100ms);assert(callback-first<900ms);
      assert(cv.wait_for(l,3s,[&]{return callbacks==2;}));
      std::cout<<"PASS queued chunk immediate first_callback_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(callback-first).count()<<std::endl;
    }else {
      const auto began=Clock::now();p.Interrupt();assert(Clock::now()-began<100ms);
      std::this_thread::sleep_for(50ms);std::lock_guard<std::mutex> l(mu);assert(callbacks==0);
      std::cout<<"PASS interrupt_ms="<<std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-began).count()-50<<std::endl;
    }
    p.Close();
  }
  assert(unlink(fake.c_str())==0);assert(rmdir(dir)==0);
}
