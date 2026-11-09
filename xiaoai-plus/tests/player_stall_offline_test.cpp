#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

#include "audio/player.hpp"

using namespace std::chrono_literals;

int main() {
  char dirname[] = "/tmp/oh2p-fake-aplay-XXXXXX";
  assert(mkdtemp(dirname) != nullptr);
  const std::string fake = std::string(dirname) + "/aplay";
  {
    std::ofstream out(fake);
    out << "#!/bin/sh\nexec sleep 30\n";
  }
  assert(chmod(fake.c_str(), 0755) == 0);
  const std::string path = std::string(dirname) + ":/usr/bin:/bin";
  assert(setenv("PATH", path.c_str(), 1) == 0);

  xiaoai_plus::config::Audio audio;
  audio.output_device = "ignored";
  audio.sample_rate = 24000;
  audio.channels = 1;
  audio.bits_per_sample = 16;
  xiaoai_plus::audio::AplayPlayer player(audio, 2);
  const auto start = std::chrono::steady_clock::now();
  assert(player.Start());
  assert(player.Play(std::vector<uint8_t>(2 * 1024 * 1024)));
  std::this_thread::sleep_for(100ms);
  player.Interrupt();
  player.Close();
  assert(std::chrono::steady_clock::now() - start < 5s);

  assert(unlink(fake.c_str()) == 0);
  // Exhaust the process FD budget so OpenPipeLocked's pipe() fails before
  // spawning aplay. The dropped chunk must still notify App.
  struct rlimit original_limit {};
  assert(getrlimit(RLIMIT_NOFILE, &original_limit) == 0);
  struct rlimit tiny_limit = original_limit;
  tiny_limit.rlim_cur = 3;
  assert(setrlimit(RLIMIT_NOFILE, &tiny_limit) == 0);
  std::mutex callback_mu;
  std::condition_variable callback_cv;
  int callbacks = 0;
  bool last_success = true;
  {
    xiaoai_plus::audio::AplayPlayer failed_player(audio, 2);
    failed_player.SetOnChunkProcessed([&](bool success) {
      std::lock_guard<std::mutex> lock(callback_mu);
      ++callbacks;
      last_success = success;
      callback_cv.notify_one();
    });
    assert(failed_player.Start());
    assert(failed_player.Play(std::vector<uint8_t>(640)));
    {
      std::unique_lock<std::mutex> lock(callback_mu);
      assert(callback_cv.wait_for(lock, 2s, [&]() { return callbacks == 1; }));
      assert(!last_success);
    }
    failed_player.Close();
  }
  assert(setrlimit(RLIMIT_NOFILE, &original_limit) == 0);
  assert(callbacks == 1);
  assert(rmdir(dirname) == 0);
}
