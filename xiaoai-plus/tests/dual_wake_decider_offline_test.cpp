#include "wakeup/dual_wake_decider.hpp"
#include "wakeup/trigger.hpp"

#include <cassert>
#include <chrono>
#include <string>
#include <vector>

int main() {
  using xiaoai_plus::wakeup::DualWakeDecider;
  using xiaoai_plus::wakeup::Trigger;
  using namespace std::chrono_literals;
  const DualWakeDecider::Time t{};

  // Combined lexicon: a long model hit wins over the short prefix.
  DualWakeDecider combined(true);
  assert(!combined.OnHit("豆包", t));
  assert(!combined.Tick(t + 500ms));
  assert(combined.OnHit("豆包豆包", t + 700ms) == "豆包豆包");
  assert(!combined.OnHit("豆包", t + 1000ms));
  assert(!combined.Tick(t + 1200ms));

  // Model resets after first short hit: a second short hit is one wake.
  combined.Reset();
  assert(!combined.OnHit("豆包", t));
  assert(combined.OnHit("豆包", t + 650ms) == "豆包");
  assert(!combined.OnHit("豆包", t + 1250ms));

  // A late second hit, without intervening Tick, releases just one wake.
  combined.Reset();
  assert(!combined.OnHit("豆包", t));
  assert(combined.OnHit("豆包", t + 1101ms) == "豆包");
  assert(!combined.OnHit("豆包", t + 1600ms));

  // A single short hit is released after the bounded hold.
  combined.Reset();
  assert(!combined.OnHit("豆包", t));
  assert(!combined.Tick(t + 1099ms));
  assert(combined.Tick(t + 1100ms) == "豆包");
  assert(!combined.OnHit("豆包豆包", t + 1500ms));

  // Short-only lexicon: first word wakes promptly; second half is suppressed.
  DualWakeDecider short_only(false);
  assert(short_only.OnHit("豆包", t) == "豆包");
  assert(!short_only.OnHit("豆包", t + 800ms));
  assert(short_only.OnHit("豆包", t + 1500ms) == "豆包");

  // Long-only lexicon has no prefix competition.
  DualWakeDecider long_only(false);
  assert(long_only.OnHit("豆包豆包", t) == "豆包豆包");

  // The rule does not delay or suppress unrelated wake labels.
  assert(long_only.OnHit("豆包同学", t + 100ms) == "豆包同学");

  std::vector<std::string> labels;
  Trigger trigger({"豆包", "豆包豆包"}, [&](const std::string& label) {
    labels.push_back(label);
    return true;
  });
  assert(trigger.Allows("豆包"));
  assert(trigger.Allows("豆包豆包"));
  assert(!trigger.Allows("豆包同学"));
  assert(trigger.FireFromText("豆包豆包"));
  assert(labels.size() == 1 && labels[0] == "豆包豆包");
}
