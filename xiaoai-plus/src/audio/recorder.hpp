#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "audio/capture_stamp.hpp"

namespace xiaoai_plus::audio {

class ArecordRecorder {
 public:
  explicit ArecordRecorder(config::Audio cfg);
  ~ArecordRecorder();

  bool Start(std::function<void(const std::vector<uint8_t>&)> on_chunk,
             std::function<void()> on_failure,
             std::function<void(const char*, size_t)> on_stderr = {});
  void Stop();
  bool StartStamped(std::function<void(const std::vector<uint8_t>&, const CaptureStamp&)> on_chunk,
                    std::function<void()> on_failure,
                    std::function<void(const char*, size_t)> on_stderr = {});
  uint64_t Generation() const { return generation_.load(); }

 private:
  void ReadLoop(std::function<void(const std::vector<uint8_t>&,const CaptureStamp&)> on_chunk,
                std::function<void()> on_failure);
  void ReadStderrLoop(std::function<void(const char*, size_t)> on_stderr);

  config::Audio cfg_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> generation_{0};
  std::atomic<bool> capture_fault_{false};
  int pipe_fd_{-1};
  int stderr_fd_{-1};
  int child_pid_{-1};
  std::thread thread_;
  std::thread stderr_thread_;
};

}  // namespace xiaoai_plus::audio
