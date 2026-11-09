#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace xiaoai_plus::wakeup {

// Coalesces overlapping @豆包 and @豆包豆包 KWS hits into one wake event.
// It never reads audio; call Tick for every processed audio chunk.
class DualWakeDecider {
 public:
  using Clock = std::chrono::steady_clock;
  using Time = Clock::time_point;

  // If both labels are in the active keyword file, wait briefly for the long
  // phrase to win. With only one label, emit immediately to avoid needless lag.
  explicit DualWakeDecider(bool both_labels,
                           std::chrono::milliseconds prefix_hold =
                               std::chrono::milliseconds(1100),
                           std::chrono::milliseconds duplicate_quiet =
                               std::chrono::milliseconds(1400));

  std::optional<std::string> OnHit(std::string_view label, Time now);
  std::optional<std::string> Tick(Time now);
  void Reset();

 private:
  std::optional<std::string> Emit(std::string label, Time now);

  bool both_labels_;
  std::chrono::milliseconds prefix_hold_;
  std::chrono::milliseconds duplicate_quiet_;
  std::optional<Time> pending_short_;
  std::optional<Time> quiet_until_;
};

}  // namespace xiaoai_plus::wakeup
