#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "audio/player.hpp"
#include "config/config.hpp"
#include "native_speech/source.hpp"
#include "realtime/client.hpp"

namespace xiaoai_plus::app {

// One native XiaoAI speech frontend and one Doubao duplex session at a time.
// This app does not open ALSA capture, load KWS models, or run a local AEC.
class NativeApp {
 public:
  using FollowUpRequester = std::function<bool()>;
  explicit NativeApp(config::Config cfg, bool trial_once = false,
                     native_speech::Source::Options source_options = {},
                     FollowUpRequester request_followup = {},
                     std::chrono::seconds registration_timeout = std::chrono::seconds(10),
                     std::chrono::seconds cloud_start_timeout = std::chrono::seconds(12),
                     std::chrono::seconds answer_timeout = std::chrono::seconds(45));
  ~NativeApp();

  bool Run();
  void RequestStop();

 private:
  enum class Phase { kWaitingWake, kCapturing, kWaitingAnswer, kWaitingFollowUp };
  enum class EventKind {
    kReady, kNativeTurn, kNativePcm, kNativeEnd, kNativeCancel, kNativeFatal,
    kCloudAudioDone, kCloudTurnDone, kCloudClosed, kAsrFinal,
    kPlaybackDone, kPlaybackFailed
  };
  struct Event {
    EventKind kind;
    explicit Event(EventKind value) : kind(value) {}
    uint64_t turn_id{0};
    native_speech::Source::Activation activation{
        native_speech::Source::Activation::kUnknown};
    std::vector<uint8_t> pcm;
    std::string text;
  };

  void Queue(Event event);
  void Process(Event event);
  void MaybeAdvanceAfterAnswer();
  void EndConversation(const char* reason);
  void DiscardCompletedCloudEvents();
  void Fail(const char* reason);
  void Cleanup();
  bool RequestFollowUp();

  config::Config cfg_;
  bool trial_once_{false};
  native_speech::Source::Options source_options_;
  FollowUpRequester request_followup_;
  std::chrono::seconds registration_timeout_;
  std::chrono::seconds cloud_start_timeout_;
  std::chrono::seconds answer_timeout_;
  std::unique_ptr<audio::AplayPlayer> player_;
  std::unique_ptr<realtime::Client> client_;
  std::unique_ptr<native_speech::Source> source_;

  std::mutex event_mu_;
  std::condition_variable event_cv_;
  std::deque<Event> events_;
  size_t queued_pcm_bytes_{0};
  bool queue_overflow_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<size_t> pending_playback_{0};
  std::atomic<bool> accept_playback_{false};
  bool failed_{false};
  bool native_registered_{false};

  Phase phase_{Phase::kWaitingWake};
  bool cloud_session_active_{false};
  bool audio_done_{false};
  bool response_done_{false};
  bool end_after_answer_{false};
  uint64_t current_turn_id_{0};
  std::chrono::steady_clock::time_point absolute_deadline_{};
  std::chrono::steady_clock::time_point idle_deadline_{};
  std::chrono::steady_clock::time_point registration_deadline_{};
  std::chrono::steady_clock::time_point answer_deadline_{};
};

}  // namespace xiaoai_plus::app
