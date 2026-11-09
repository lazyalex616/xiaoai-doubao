#include <cassert>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>
#include <unistd.h>

#include "app/native_wake_monitor.hpp"

int main() {
  int wake_count = 0;
  xiaoai_plus::app::NativeWakeMonitor monitor([&] { ++wake_count; });
  assert(!monitor.Blocked());
  monitor.ObserveLine("OH2P mediaplayer: send real_wakeup event");
  assert(!monitor.Blocked());
  monitor.ObserveLine("OH2P mipns-xiaomi[2425]: send pre_wakeup event");
  assert(!monitor.Blocked());
  monitor.ObserveLine("OH2P mipns-xiaomi[2425]: send real_wakeup event");
  assert(monitor.Blocked() && wake_count == 1);
  monitor.ObserveLine("OH2P mipns-xiaomi[2425]: send real_wakeup event");
  assert(wake_count == 1);
  monitor.ObserveLine("OH2P wakeup.sh: asrend");
  assert(monitor.Blocked());  // Native release gets a short grace period.
  monitor.ObserveLine("OH2P wakeup.sh: asrstart");
  assert(monitor.Blocked() && wake_count == 2);

  char path[] = "/tmp/native-wake-monitor-XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  std::atomic<int> live_wake{0};
  {
    xiaoai_plus::app::NativeWakeMonitor live([&] { ++live_wake; }, path);
    live.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    { std::ofstream out(path, std::ios::app);
      out << "OH2P mipns-xiaomi[2425]: send real_wakeup event\n"; }
    for (int i = 0; i < 20 && live_wake.load() == 0; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(live_wake.load() == 1 && live.Blocked());
    live.Stop();
  }
  unlink(path);
}
