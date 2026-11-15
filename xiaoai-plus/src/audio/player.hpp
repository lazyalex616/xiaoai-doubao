#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <sys/types.h>
#include <thread>
#include <vector>

#include "config/config.hpp"

namespace xiaoai_plus::audio {

class AplayPlayer {
 public:
  AplayPlayer(config::Audio cfg, int queue_frames);
  ~AplayPlayer();

  bool Start();
  enum class ChunkResult { kWritten, kCanceled, kFailed };
  uint64_t Generation() const { return generation_.load(); }
  bool Play(const std::vector<uint8_t>& chunk);
  // Local cues must not consume cloud playback completion callbacks.
  bool Play(const std::vector<uint8_t>& chunk, uint64_t generation, bool notify = true);
  void Interrupt();
  // False for a stale request; never cancels newer playback.
  bool Interrupt(uint64_t generation);
  // After queued writes finish, close stdin and let aplay play its buffered tail.
  bool Drain(uint64_t generation, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
  void SetOnScopedChunkProcessed(std::function<void(uint64_t, ChunkResult)> cb);
  void Close();

  // Called after a chunk and its audible tail drain, or it fails; queued audio skips the wait.
  void SetOnChunkProcessed(std::function<void(bool)> cb);
  void SetOnWriteProgress(std::function<void()> cb);
  void SetOnFatal(std::function<void()> cb);

 private:
  bool OpenPipeLocked();
  void ClosePipeLocked(bool kill_now);
  void WriteLoop();
  void WaitAudible(uint64_t generation);

  config::Audio cfg_;
  int queue_frames_{128};

  std::mutex mu_;
  std::condition_variable cv_;
  struct Chunk { std::vector<uint8_t> bytes; uint64_t generation; bool notify{true}; };
  std::deque<Chunk> queue_;
  bool writing_{false};
  std::atomic<uint64_t> generation_{1};
  std::recursive_mutex callback_fence_;

  std::mutex cb_mu_;
  std::function<void(bool)> on_chunk_processed_;
  std::function<void(uint64_t, ChunkResult)> on_scoped_chunk_processed_;
  std::function<void()> on_write_progress_;
  std::function<void()> on_fatal_;

  std::atomic<bool> running_{false};
  std::mutex pipe_mu_;
  int aplay_fd_{-1};
  pid_t aplay_pid_{-1};
  uint64_t pipe_generation_{0};
  std::thread thread_;
};

}  // namespace xiaoai_plus::audio
