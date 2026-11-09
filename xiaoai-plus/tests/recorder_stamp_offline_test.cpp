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
#include "audio/recorder.hpp"
using namespace std::chrono_literals;
int main() {
  char path[]="/tmp/xz-capture-stamp-XXXXXX";assert(mkdtemp(path));
  const std::string script=std::string(path)+"/arecord";
  {std::ofstream out(script);out<<"#!/usr/bin/env python3\nimport sys,time\n"
    "block=bytes(1440*4*4)\n"
    "for i in range(100):\n"
    " sys.stdout.buffer.write(block);sys.stdout.buffer.flush()\n"
    " if i==1: sys.stderr.write('overrun!!!\\n');sys.stderr.flush()\n"
    " time.sleep(.09)\n";}
  assert(chmod(script.c_str(),0700)==0);
  const std::string old=getenv("PATH")?getenv("PATH"):"";
  assert(setenv("PATH",(std::string(path)+":"+old).c_str(),1)==0);
  xiaoai_plus::config::Audio cfg;cfg.channels=4;cfg.bits_per_sample=32;cfg.sample_rate=16000;
  cfg.buffer_size=1440;cfg.period_size=360;
  xiaoai_plus::audio::ArecordRecorder recorder(cfg);
  std::mutex mutex;std::condition_variable cv;
  std::vector<xiaoai_plus::audio::CaptureStamp> stamps;bool failed=false;
  auto start=[&] {return recorder.StartStamped([&](const auto& bytes,const auto& stamp) {
    assert(bytes.size()==23040);std::lock_guard<std::mutex> lock(mutex);stamps.push_back(stamp);cv.notify_all();
  },[&] {std::lock_guard<std::mutex> lock(mutex);failed=true;cv.notify_all();},[](const char*,size_t) {});};
  assert(start());const auto generation=recorder.Generation();assert(generation==1);
  assert(start() && recorder.Generation()==generation); // No fake restart while alive.
  {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,3s,[&] {return stamps.size()>=4 || failed;}));assert(!failed);}
  recorder.Stop();
  assert(stamps.size()>=4 && stamps[0].generation==1 && stamps[0].first_sample==0);
  assert(stamps[0].continuous);
  bool fault_seen=false;
  for(size_t i=0;i<stamps.size();++i) {
    assert(stamps[i].generation==generation && stamps[i].first_sample==i*1440);
    assert(stamps[i].first_read_ns && stamps[i].first_read_ns<=stamps[i].delivered_ns);
    if(i)assert(stamps[i].delivered_ns>=stamps[i-1].delivered_ns);
    if(!stamps[i].continuous)fault_seen=true;
    if(fault_seen)assert(!stamps[i].continuous); // Overrun cannot relabel old data as healthy.
  }
  assert(fault_seen);
  {std::lock_guard<std::mutex> lock(mutex);stamps.clear();}
  assert(start());assert(recorder.Generation()==generation+1);
  {std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,3s,[&] {return stamps.size()>=1 || failed;}));assert(!failed);}
  recorder.Stop();
  assert(stamps[0].generation==2 && stamps[0].first_sample==0 && stamps[0].continuous);
  assert(setenv("PATH",old.c_str(),1)==0);unlink(script.c_str());rmdir(path);
  std::cout<<"PASS actual recorder fork/read/stop/join/restart local producer provenance; physical microphone not exercised\n";
}
