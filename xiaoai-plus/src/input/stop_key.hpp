#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace xiaoai_plus::input {

// Read-only evdev subscriber. Never grabs, injects or consumes another reader's events.
class StopKey {
 public:
  StopKey(std::string device, int key_code, std::function<void()> on_press);
  ~StopKey();
  bool Start();
  void Stop();

 private:
  void ReadLoop();
  std::string device_;
  int key_code_, fd_{-1};
  std::function<void()> on_press_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};
}  // namespace xiaoai_plus::input
