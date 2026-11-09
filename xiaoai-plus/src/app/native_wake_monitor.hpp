#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xiaoai_plus::app {

// Follows the existing OH2P syslog. Never reads historic lines on startup.
class NativeWakeMonitor {
 public:
  using Clock = std::chrono::steady_clock;
  explicit NativeWakeMonitor(std::function<void()> on_wake,
                             std::string path = "/tmp/log/messages");
  ~NativeWakeMonitor();
  void Start();
  void Stop();
  void ObserveLine(const std::string& line);
  bool Blocked() const;

 private:
  void Loop();
  std::function<void()> on_wake_;
  std::string path_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  mutable std::mutex mu_;
  bool native_active_{false};
  Clock::time_point hold_until_{};
};

}  // namespace xiaoai_plus::app
