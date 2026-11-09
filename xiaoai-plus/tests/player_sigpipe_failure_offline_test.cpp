#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "audio/player.hpp"

using namespace std::chrono_literals;

int main() {
  xiaoai_plus::config::Audio audio;
  audio.output_device = "ignored";
  std::mutex mu;
  std::condition_variable cv;
  int failures = 0;
  {
    xiaoai_plus::audio::AplayPlayer player(audio, 2);
    player.SetOnChunkProcessed([&](bool success) {
      std::lock_guard<std::mutex> lock(mu);
      assert(!success);
      ++failures;
      cv.notify_one();
    });
    assert(player.Start());
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] { return failures == 1; }));
    lock.unlock();
    player.Close();  // Must join even though the writer already set running=false.
  }
  assert(failures == 1);
}
