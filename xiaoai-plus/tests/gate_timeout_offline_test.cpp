#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "wakeup/gate.hpp"

using namespace std::chrono_literals;

int main() {
  std::mutex mu;
  std::condition_variable cv;
  std::string reason;
  xiaoai_plus::wakeup::Hooks hooks;
  hooks.after_disarm = [&](const std::string& value) {
    std::lock_guard<std::mutex> lock(mu);
    reason = value;
    cv.notify_all();
  };
  xiaoai_plus::wakeup::Gate gate(70ms, hooks, 180ms);
  assert(gate.TryWakeup("豆包"));
  gate.SetAiSpeaking(true);  // The normal inactivity timer is paused.
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 700ms, [&] { return !reason.empty(); }));
    assert(reason == "absolute_timeout");
  }
  assert(gate.step() == xiaoai_plus::wakeup::Step::kIdle);
  reason.clear();
  assert(gate.TryWakeup("豆包"));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 700ms, [&] { return !reason.empty(); }));
    assert(reason == "timeout");
  }
  gate.Close();
  assert(!gate.TryWakeup("豆包"));
  reason.clear();
  xiaoai_plus::wakeup::Gate unlimited(120ms, hooks);
  assert(unlimited.TryWakeup("豆包"));
  for (int i = 0; i < 4; ++i) {
    std::this_thread::sleep_for(65ms);
    unlimited.RefreshTimeout();  // Activity spans the old absolute deadline.
    assert(unlimited.step() == xiaoai_plus::wakeup::Step::kActive);
  }
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 400ms, [&] { return !reason.empty(); }));
    assert(reason == "timeout");
  }
  unlimited.Close();

  // Missing output_audio.done, a lost playback callback, and a silent stream
  // have the same observable state: speaking without received or written audio.
  int recovered = 0;
  xiaoai_plus::wakeup::Hooks stall_hooks;
  stall_hooks.on_speaking_stall = [&] {
    std::lock_guard<std::mutex> lock(mu);
    ++recovered;
    cv.notify_all();
  };
  xiaoai_plus::wakeup::Gate stalled(180ms, stall_hooks, 0ms, 70ms);
  assert(stalled.TryWakeup("豆包"));
  stalled.SetAiSpeaking(true);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 500ms, [&] { return recovered == 1; }));
  }
  assert(!stalled.IsAiSpeaking());
  assert(stalled.step() == xiaoai_plus::wakeup::Step::kActive);
  stalled.Close();

  recovered = 0;
  xiaoai_plus::wakeup::Gate streaming(120ms, stall_hooks, 0ms, 70ms);
  assert(streaming.TryWakeup("豆包"));
  streaming.SetAiSpeaking(true);
  for (int i = 0; i < 8; ++i) {
    std::this_thread::sleep_for(30ms);
    streaming.NoteAiProgress();  // Continuous incoming audio or player writes.
    assert(streaming.IsAiSpeaking());
  }
  assert(recovered == 0);
  streaming.SetAiSpeaking(false);
  streaming.Close();
  // A cloud transcription pauses idle expiry, but has its own bounded lifetime.
  using xiaoai_plus::wakeup::Gate;
  using xiaoai_plus::wakeup::Step;
  reason.clear();
  Gate user_turn(100ms, hooks, 0ms, 500ms, 350ms);
  user_turn.SetUserTurnOpen(true);  // Ignored while idle.
  user_turn.Activate("test");
  user_turn.SetUserTurnOpen(true);
  std::this_thread::sleep_for(160ms);
  assert(user_turn.step()==Step::kActive);
  user_turn.SetUserTurnOpen(false);
  std::this_thread::sleep_for(45ms);
  assert(user_turn.step()==Step::kActive);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 300ms, [&]{return reason=="timeout";}));
  }
  reason.clear();
  assert(user_turn.TryWakeup("豆包"));
  user_turn.SetUserTurnOpen(true);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(!cv.wait_for(lock, 160ms, [&]{return !reason.empty();}));
    assert(cv.wait_for(lock, 400ms, [&]{return reason=="timeout";}));
  }
  reason.clear();
  user_turn.Activate("test");
  user_turn.SetUserTurnOpen(true);
  user_turn.SetAiSpeaking(true);  // Output clears the open input turn.
  std::this_thread::sleep_for(150ms);
  assert(user_turn.step()==Step::kActive);
  user_turn.SetAiSpeaking(false);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 250ms, [&]{return reason=="timeout";}));
  }
  reason.clear();
  user_turn.Activate("test");
  user_turn.SetUserTurnOpen(true);
  user_turn.Disarm("test");
  reason.clear();
  assert(user_turn.TryWakeup("豆包"));  // CommitWakeup clears a previous open turn.
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 250ms, [&]{return reason=="timeout";}));
  }
  reason.clear();
  user_turn.SetUserTurnOpen(true);  // Still idle: opening does not survive Activate.
  user_turn.Activate("test");
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 250ms, [&]{return reason=="timeout";}));
  }
  user_turn.Close();
}
