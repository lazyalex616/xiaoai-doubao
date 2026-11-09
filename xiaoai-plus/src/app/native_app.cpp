#include "app/native_app.hpp"
#include <signal.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "app/conversation_policy.hpp"
#include "common/log.hpp"

extern char** environ;

namespace xiaoai_plus::app {
namespace {
const auto kLog = GetLogger("native_app");
using Clock = std::chrono::steady_clock;
constexpr size_t kMaxQueuedPcm = 8 * 16000 * 2;
constexpr size_t kMaxEvents = 2048;

void SaturatingDecrement(std::atomic<size_t>* value) {
  size_t pending = value->load();
  while (pending > 0 && !value->compare_exchange_weak(pending, pending - 1)) {}
}

bool RequestPnsFollowUpBounded() {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int pipefd[2];
  if (pipe(pipefd) != 0) return false;
  const int flags = fcntl(pipefd[0], F_GETFL);
  if (flags < 0 || fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK) != 0) {
    close(pipefd[0]); close(pipefd[1]); return false;
  }
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(pipefd[0]); close(pipefd[1]); return false;
  }
  posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  char a0[] = "ubus", a1[] = "-t", a2[] = "1", a3[] = "-S";
  char a4[] = "call", a5[] = "pnshelper", a6[] = "event_notify";
  char a7[] = "{\"src\":1,\"event\":0}";
  char* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7, nullptr};
  pid_t pid = -1;
  const int spawned = posix_spawnp(&pid, "ubus", &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipefd[1]);
  if (spawned != 0 || pid <= 0) { close(pipefd[0]); return false; }

  std::string output;
  int status = 0;
  bool exited = false;
  const auto deadline = Clock::now() + std::chrono::seconds(2);
  while (Clock::now() < deadline) {
    char buffer[256];
    const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
    if (count > 0) {
      if (output.size() + static_cast<size_t>(count) > 1024) break;
      output.append(buffer, static_cast<size_t>(count));
    } else if (count < 0 && errno != EAGAIN && errno != EINTR) {
      break;
    }
    const pid_t waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) { exited = true; break; }
    if (waited < 0) break;
    pollfd fd{pipefd[0], POLLIN, 0};
    (void)poll(&fd, 1, 50);
  }
  if (!exited) {
    kill(pid, SIGKILL);
    for (int i = 0; i < 20; ++i) {
      const pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid || waited < 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  // Drain once after child exit; never log the remote payload.
  char buffer[256];
  for (;;) {
    const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
    if (count <= 0) break;
    if (output.size() + static_cast<size_t>(count) > 1024) break;
    output.append(buffer, static_cast<size_t>(count));
  }
  close(pipefd[0]);
  if (!exited || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
  try {
    const auto reply = nlohmann::json::parse(output);
    return reply.is_object() && reply.contains("code") &&
           reply["code"].is_number_integer() && reply["code"].get<int>() == 0;
  } catch (...) { return false; }
}
}  // namespace

NativeApp::NativeApp(config::Config cfg, bool trial_once,
                     native_speech::Source::Options source_options,
                     FollowUpRequester request_followup,
                     std::chrono::seconds registration_timeout,
                     std::chrono::seconds cloud_start_timeout,
                     std::chrono::seconds answer_timeout)
    : cfg_(std::move(cfg)), trial_once_(trial_once),
      source_options_(std::move(source_options)),
      request_followup_(request_followup ? std::move(request_followup)
                                         : RequestPnsFollowUpBounded),
      registration_timeout_(registration_timeout),
      cloud_start_timeout_(cloud_start_timeout),
      answer_timeout_(answer_timeout) {
  auto playback_cfg = config::MakeTtsPlaybackAudio(cfg_.audio);
  playback_cfg.output_device = "notify";
  player_ = std::make_unique<audio::AplayPlayer>(playback_cfg,
                                                 cfg_.budget.output_queue_frames);
  player_->SetOnChunkProcessed([this](bool success) {
    SaturatingDecrement(&pending_playback_);
    Queue(Event{success ? EventKind::kPlaybackDone : EventKind::kPlaybackFailed});
  });
  realtime::Client::Callbacks callbacks;
  callbacks.on_audio = [this](const std::vector<uint8_t>& pcm) {
    if (!accept_playback_.load()) return;
    ++pending_playback_;
    if (!player_->Play(pcm)) {
      SaturatingDecrement(&pending_playback_);
      Queue(Event{EventKind::kPlaybackFailed});
    }
  };
  callbacks.on_audio_done = [this] { Queue(Event{EventKind::kCloudAudioDone}); };
  callbacks.on_chat_ended = [this] { Queue(Event{EventKind::kCloudTurnDone}); };
  callbacks.on_asr_final = [this](const std::string& text) {
    Event event{EventKind::kAsrFinal};
    event.text = text;
    Queue(std::move(event));
  };
  callbacks.on_session_closed = [this](const std::string&) {
    Queue(Event{EventKind::kCloudClosed});
  };
  client_ = std::make_unique<realtime::Client>(cfg_, callbacks, trial_once_, true);
  source_ = std::make_unique<native_speech::Source>(source_options_);
}

NativeApp::~NativeApp() { RequestStop(); }

void NativeApp::Queue(Event event) {
  std::lock_guard<std::mutex> lock(event_mu_);
  if (stop_requested_.load()) return;
  if (events_.size() >= kMaxEvents ||
      (event.kind == EventKind::kNativePcm &&
       queued_pcm_bytes_ + event.pcm.size() > kMaxQueuedPcm)) {
    queue_overflow_ = true;
    event_cv_.notify_one();
    return;
  }
  queued_pcm_bytes_ += event.pcm.size();
  events_.push_back(std::move(event));
  event_cv_.notify_one();
}

void NativeApp::RequestStop() {
  stop_requested_.store(true);
  event_cv_.notify_all();
}

bool NativeApp::Run() {
  if (running_.exchange(true)) return false;
  if (!player_->Start() || !client_->Start()) {
    Fail("components_start_failed");
    Cleanup();
    return false;
  }
  native_speech::Source::Callbacks callbacks;
  callbacks.on_ready = [this](const std::string&, const std::string&) {
    Queue(Event{EventKind::kReady});
  };
  callbacks.on_turn = [this](uint64_t id, native_speech::Source::Activation mode) {
    Event event{EventKind::kNativeTurn}; event.turn_id = id; event.activation = mode;
    Queue(std::move(event));
  };
  callbacks.on_pcm = [this](uint64_t id, const std::vector<uint8_t>& pcm) {
    Event event{EventKind::kNativePcm}; event.turn_id = id; event.pcm = pcm;
    Queue(std::move(event));
  };
  callbacks.on_end = [this](uint64_t id) { Event e{EventKind::kNativeEnd}; e.turn_id = id; Queue(e); };
  callbacks.on_cancel = [this](uint64_t id) { Event e{EventKind::kNativeCancel}; e.turn_id = id; Queue(e); };
  callbacks.on_fatal = [this](const std::string&) { Queue(Event{EventKind::kNativeFatal}); };
  if (!source_->Start(std::move(callbacks))) {
    Fail("native_socket_start_failed");
    Cleanup();
    return false;
  }
  registration_deadline_ = Clock::now() + registration_timeout_;
  kLog->info("native speech socket bound; waiting for PCM registration");

  while (!stop_requested_.load()) {
    Event event{EventKind::kReady};
    bool have_event = false;
    bool overflow = false;
    {
      std::unique_lock<std::mutex> lock(event_mu_);
      event_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
        return stop_requested_.load() || queue_overflow_ || !events_.empty();
      });
      overflow = queue_overflow_;
      if (!events_.empty()) {
        event = std::move(events_.front());
        queued_pcm_bytes_ -= event.pcm.size();
        events_.pop_front();
        have_event = true;
      }
    }
    if (overflow) { Fail("native_event_queue_overflow"); break; }
    const auto now = Clock::now();
    if (!native_registered_ && !source_->registered() && now >= registration_deadline_) {
      Fail("native_pcm_registration_timeout");
    } else if (cloud_session_active_ && absolute_deadline_ != Clock::time_point{} &&
        now >= absolute_deadline_) {
      EndConversation("absolute_limit");
    } else if (phase_ == Phase::kWaitingAnswer &&
               answer_deadline_ != Clock::time_point{} && now >= answer_deadline_) {
      EndConversation("answer_timeout");
    } else if (phase_ == Phase::kWaitingFollowUp &&
               idle_deadline_ != Clock::time_point{} && now >= idle_deadline_) {
      EndConversation("followup_timeout");
    }
    if (have_event && !stop_requested_.load()) Process(std::move(event));
  }
  Cleanup();
  return !failed_;
}

void NativeApp::Process(Event event) {
  switch (event.kind) {
    case EventKind::kReady:
      native_registered_ = true;
      kLog->info("native PCM frontend registered");
      break;
    case EventKind::kNativeTurn:
      if (event.activation == native_speech::Source::Activation::kWake &&
          phase_ == Phase::kWaitingWake) {
        if (!player_->Start()) { Fail("player_start_failed"); return; }
        current_turn_id_ = event.turn_id;
        absolute_deadline_ = Clock::now() + std::chrono::minutes(3);
        idle_deadline_ = {};
        answer_deadline_ = {};
        audio_done_ = response_done_ = end_after_answer_ = false;
        if (!client_->StartSession(cloud_start_timeout_)) {
          Fail("cloud_session_start_failed");
          return;
        }
        if (stop_requested_.load()) return;
        cloud_session_active_ = true;
        accept_playback_.store(true);
        phase_ = Phase::kCapturing;
      } else if (event.activation == native_speech::Source::Activation::kFollowUp &&
                 phase_ == Phase::kWaitingFollowUp && cloud_session_active_ &&
                 event.turn_id > current_turn_id_) {
        current_turn_id_ = event.turn_id;
        idle_deadline_ = {};
        answer_deadline_ = {};
        audio_done_ = response_done_ = end_after_answer_ = false;
        phase_ = Phase::kCapturing;
      } else {
        kLog->error("unexpected native turn; closing takeover");
        (void)source_->StopCapture(event.turn_id);
        Fail("unexpected_native_turn");
      }
      break;
    case EventKind::kNativePcm:
      if (phase_ == Phase::kCapturing && event.turn_id == current_turn_id_ &&
          !client_->EnqueueAudio(std::move(event.pcm))) Fail("native_pcm_enqueue_failed");
      break;
    case EventKind::kNativeEnd:
      if (phase_ == Phase::kCapturing && event.turn_id == current_turn_id_) {
        if (!client_->EndInputTurn()) { Fail("native_input_end_failed"); return; }
        phase_ = Phase::kWaitingAnswer;
        answer_deadline_ = Clock::now() + answer_timeout_;
        MaybeAdvanceAfterAnswer();
      }
      break;
    case EventKind::kNativeCancel:
      if (event.turn_id == current_turn_id_ && phase_ != Phase::kWaitingWake)
        EndConversation("native_cancel");
      break;
    case EventKind::kNativeFatal:
      Fail("native_frontend_fatal");
      break;
    case EventKind::kCloudAudioDone:
      audio_done_ = true;
      MaybeAdvanceAfterAnswer();
      break;
    case EventKind::kCloudTurnDone:
      response_done_ = true;
      MaybeAdvanceAfterAnswer();
      break;
    case EventKind::kCloudClosed:
      if (cloud_session_active_) Fail("cloud_session_closed");
      break;
    case EventKind::kAsrFinal:
      if (IsExitCommand(event.text)) {
        EndConversation("user_exit");
      } else if (IsFarewellCommand(event.text)) {
        end_after_answer_ = true;
      }
      break;
    case EventKind::kPlaybackDone:
      MaybeAdvanceAfterAnswer();
      break;
    case EventKind::kPlaybackFailed:
      Fail("audio_playback_failed");
      break;
  }
}

void NativeApp::MaybeAdvanceAfterAnswer() {
  if (phase_ != Phase::kWaitingAnswer || !audio_done_ || !response_done_ ||
      pending_playback_.load() != 0) return;
  if (end_after_answer_) { EndConversation("farewell"); return; }
  if (!source_->ContinueDialog(current_turn_id_) || !RequestFollowUp()) {
    Fail("native_followup_failed");
    return;
  }
  phase_ = Phase::kWaitingFollowUp;
  answer_deadline_ = {};
  idle_deadline_ = Clock::now() + std::chrono::seconds(cfg_.conversation.idle_seconds);
  kLog->info("next native turn ready; idle_window_seconds={}", cfg_.conversation.idle_seconds);
}

bool NativeApp::RequestFollowUp() {
  return request_followup_ && request_followup_();
}

void NativeApp::EndConversation(const char* reason) {
  kLog->info("native conversation ended: {}", reason);
  accept_playback_.store(false);
  bool native_ok = true;
  if (current_turn_id_) {
    if (phase_ == Phase::kCapturing)
      native_ok = source_->StopCapture(current_turn_id_);
    native_ok = source_->FinishDialog(current_turn_id_) && native_ok;
  }
  cloud_session_active_ = false;
  if (client_) (void)client_->FinishSession(std::chrono::seconds(2));
  // Close joins the old writer/callback before a later Wake can use the
  // player again; stale processed chunks cannot decrement a new turn.
  if (player_) player_->Close();
  DiscardCompletedCloudEvents();
  phase_ = Phase::kWaitingWake;
  absolute_deadline_ = {};
  idle_deadline_ = {};
  answer_deadline_ = {};
  current_turn_id_ = 0;
  pending_playback_.store(0);
  if (!native_ok) Fail("native_finish_failed");
}

void NativeApp::DiscardCompletedCloudEvents() {
  std::lock_guard<std::mutex> lock(event_mu_);
  events_.erase(std::remove_if(events_.begin(), events_.end(), [](const Event& event) {
    switch (event.kind) {
      case EventKind::kCloudAudioDone:
      case EventKind::kCloudTurnDone:
      case EventKind::kCloudClosed:
      case EventKind::kAsrFinal:
      case EventKind::kPlaybackDone:
      case EventKind::kPlaybackFailed:
        return true;
      default:
        return false;
    }
  }), events_.end());
}

void NativeApp::Fail(const char* reason) {
  if (failed_) return;
  failed_ = true;
  kLog->error("native app failed: {}", reason);
  RequestStop();
}

void NativeApp::Cleanup() {
  RequestStop();
  // Close the billable connection before any potentially stalled audio process.
  if (client_) client_->Stop();
  if (source_) source_->Stop();
  if (player_) player_->Close();
  running_.store(false);
}

}  // namespace xiaoai_plus::app
