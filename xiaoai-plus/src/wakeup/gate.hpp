#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace xiaoai_plus::wakeup {

enum class Step {
  kIdle,
  kActive,
};

struct Hooks {
  std::function<void(const std::string&)> after_disarm;
  std::function<void(const std::string&,uint64_t)> after_disarm_scoped;
  std::function<void(const std::string&)> on_arm;
  std::function<void(const std::string&)> on_interrupt;
  std::function<void()> on_speaking_stall;
  std::function<void()> on_fatal;
};

class Gate {
 public:
  Gate(std::chrono::milliseconds timeout, Hooks hooks,
       std::chrono::milliseconds absolute_timeout = std::chrono::milliseconds(0),
       std::chrono::milliseconds speaking_stall = std::chrono::seconds(20),
       std::chrono::milliseconds user_turn_limit = std::chrono::minutes(2));
  ~Gate();

  Step step() const;
  bool IsAiSpeaking() const;
  struct WakeCommit {
    uint64_t revision{0};
    bool interrupt{false};
    std::string reason;
    explicit operator bool() const { return revision != 0; }
  };
  // Pure state transition: no App hook/model/network effects under caller fence.
  WakeCommit CommitWakeup(const std::string& keyword);
  bool IsWakeCurrent(const WakeCommit&) const;
  void CancelWakeup(const WakeCommit&);
  struct DisarmCommit {
    uint64_t revision{0};
    std::string reason;
    explicit operator bool() const { return revision != 0; }
  };
  DisarmCommit CommitDisarm(const std::string& reason);
  bool IsDisarmCurrent(const DisarmCommit&) const;
  void DispatchDisarm(const DisarmCommit&);
  bool ClaimDisarmCleanup(const DisarmCommit&);
  void ReleaseDisarmCleanup(const DisarmCommit&);
  uint64_t Revision() const;
  bool TryWakeup(const std::string& keyword);
  void Activate(const std::string& reason);
  void Disarm(const std::string& reason);
  void RefreshTimeout();
  void SetUserTurnOpen(bool open);

  void SetAiSpeaking(bool is_speaking);
  void NoteAiProgress();
  void Close();

 private:
  void RefreshTimeoutLocked();
  void TimeoutLoop();

  std::chrono::milliseconds timeout_;
  std::chrono::milliseconds absolute_timeout_;
  std::chrono::milliseconds speaking_stall_;
  std::chrono::milliseconds user_turn_limit_;
  Hooks hooks_;

  mutable std::mutex mu_;
  std::condition_variable timeout_cv_;
  Step step_{Step::kIdle};
  bool is_ai_speaking_{false};
  bool user_turn_open_{false};
  std::chrono::steady_clock::time_point user_turn_deadline_{};
  std::chrono::steady_clock::time_point timeout_deadline_{};
  std::chrono::steady_clock::time_point absolute_deadline_{};
  std::chrono::steady_clock::time_point speaking_progress_deadline_{};
  bool closed_{false};
  uint64_t timeout_epoch_{0};
  uint64_t wake_revision_{1}, cleanup_revision_{0};
  std::thread timeout_thread_;
};

}  // namespace xiaoai_plus::wakeup
