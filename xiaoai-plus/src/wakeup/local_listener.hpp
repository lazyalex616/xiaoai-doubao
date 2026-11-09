#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "wakeup/gate.hpp"
#include "wakeup/dual_wake_decider.hpp"
#include "wakeup/kws_engine.hpp"
#include "wakeup/trigger.hpp"

namespace xiaoai_plus::wakeup {

class LocalListener {
 public:
  struct Config {
    Gate* gate{nullptr};
    Trigger* trigger{nullptr};
    std::shared_ptr<IKwsEngine> kws_engine;
    int sample_rate{24000};
    int channels{1};
    int bit_depth{16};
    int min_trigger_interval_ms{1500};
    bool allow_playback_interrupt{false};
    bool bounded_decisions{false};
    float exit_playback_threshold{0.45f};
    std::function<void()> on_end;
    std::function<bool(const std::string&)> ai_text_contains;
    std::function<bool()> exit_allowed; // Optional extra gate for goodbye only.
    // Goodbye stays off after wake, while AI speaks, and for a short tail.
    // Strong commands bypass these protections throughout an active session.
    std::chrono::milliseconds exit_after_wake{2500};
    std::chrono::milliseconds exit_after_ai_speech{500};
  };

  explicit LocalListener(Config cfg);
  struct Decision {
    std::string keyword;
    bool end_session{false};
    std::chrono::steady_clock::time_point birth{};
    bool held_prefix{false};
    std::chrono::steady_clock::time_point prefix_due{};
    explicit operator bool() const { return end_session || !keyword.empty(); }
    bool CanCommitAt(std::chrono::steady_clock::time_point now,
                     std::chrono::steady_clock::time_point current_birth) const {
      if(!*this || current_birth.time_since_epoch().count()==0 || now<current_birth ||
          now-current_birth>std::chrono::milliseconds(500))return false;
      if(held_prefix)return prefix_due.time_since_epoch().count()!=0 &&
          now<=prefix_due+std::chrono::milliseconds(500);
      return birth.time_since_epoch().count()!=0 && now>=birth &&
          now-birth<=std::chrono::milliseconds(500);
    }
  };
  void AcceptPcm(const std::vector<uint8_t>& chunk);
  // No Trigger/App effects. ordinary-on calls only on its single consumer.
  Decision AnalyzePcm(const std::vector<uint8_t>& chunk,
                     std::chrono::steady_clock::time_point birth);
  void Reset();
  void NoteAccepted();
  void Close();

 private:
  Config cfg_;
  DualWakeDecider decider_;
  std::mutex mu_;
  bool UpdateExitWordsLocked(std::chrono::steady_clock::time_point now);

  std::chrono::steady_clock::time_point last_trigger_{};
  std::chrono::steady_clock::time_point active_since_{};
  std::chrono::steady_clock::time_point ai_quiet_since_{};
  bool was_active_{false};
  bool was_exit_allowed_{false};
  bool was_ai_speaking_{false};
  bool exit_enabled_{false},strong_enabled_{false};
  bool closed_{false};
  std::chrono::steady_clock::time_point prefix_birth_{};
  std::chrono::steady_clock::time_point prefix_due_{};
};

}  // namespace xiaoai_plus::wakeup
