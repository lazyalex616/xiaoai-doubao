#include "wakeup/dual_wake_decider.hpp"

#include <utility>

namespace xiaoai_plus::wakeup {

namespace {
constexpr std::string_view kShort = "豆包";
constexpr std::string_view kLong = "豆包豆包";
}  // namespace

DualWakeDecider::DualWakeDecider(bool both_labels,
                                 std::chrono::milliseconds prefix_hold,
                                 std::chrono::milliseconds duplicate_quiet)
    : both_labels_(both_labels),
      prefix_hold_(prefix_hold),
      duplicate_quiet_(duplicate_quiet) {}

std::optional<std::string> DualWakeDecider::Emit(std::string label, Time now) {
  pending_short_.reset();
  quiet_until_ = now + duplicate_quiet_;
  return label;
}

std::optional<std::string> DualWakeDecider::OnHit(std::string_view label, Time now) {
  if (label != kShort && label != kLong) {
    pending_short_.reset();
    return std::string(label);  // Preserve existing behavior for other labels.
  }
  if (quiet_until_ && now < *quiet_until_) return std::nullopt;

  if (label == kLong) return Emit(std::string(kLong), now);

  if (label == kShort && both_labels_) {
    if (pending_short_) {
      // The model may reset on the first short hit and never return the long
      // label. A second short hit still represents one possible utterance.
      // If Tick was delayed beyond prefix_hold_ (e.g. capture stalled),
      // release the pending wake here instead of emitting twice.
      return Emit(std::string(kShort), now);
    }
    pending_short_ = now;
    return std::nullopt;
  }

  // With only @豆包 configured, the first half of “豆包豆包” is itself an
  // accepted wake; quiet time prevents a second wake from its remaining half.
  return Emit(std::string(label), now);
}

std::optional<std::string> DualWakeDecider::Tick(Time now) {
  if (pending_short_ && now - *pending_short_ >= prefix_hold_) {
    return Emit(std::string(kShort), now);
  }
  return std::nullopt;
}

void DualWakeDecider::Reset() {
  pending_short_.reset();
  quiet_until_.reset();
}

}  // namespace xiaoai_plus::wakeup
