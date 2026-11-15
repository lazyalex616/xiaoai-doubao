#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "config/config.hpp"
#include "audio/output_format.hpp"
#include "realtime/duplex_protocol.hpp"
#include "realtime/o2_protocol.hpp"
#include "realtime/daily_usage.hpp"
#include "realtime/barge_in_controller.hpp"
#include "realtime/transport_owner.hpp"
#include "audio/shadow_voice.hpp"

namespace xiaoai_plus::realtime {

class Client {
 public:
  struct Callbacks {
    std::function<void(const std::vector<uint8_t>&)> on_audio;
    std::function<void()> on_audio_done;
    std::function<void(bool)> on_set_ai_speaking;
    std::function<void(const duplex::Event&)> on_asr_turn;
    std::function<void(const std::string&)> on_asr_final;
    std::function<void()> on_asr_empty;
    std::function<void()> on_user_activity;
    std::function<void(const std::string&)> on_session_closed;
    std::function<void()> on_chat_ended;
    std::function<void()> on_exit_intent;
    std::function<void()> on_fatal;
    std::function<void(const duplex::Event&)> on_text_event;
    std::function<void(const IdentitySessionOrigin&,const duplex::Event&)> on_identity_text_event;
    std::function<void(const duplex::Event&)> on_response_text;
    // Fresh mode never invokes the untagged callbacks above. Preserve this
    // envelope in queued Player/Service/owner work; event.audio is decoded S16.
    std::function<void(const TransportEnvelope&, const duplex::Event&)> on_transport_event;
    std::function<void(const TransportEnvelope&, const std::string&)> on_transport_closed;
  };

  explicit Client(config::Config cfg, Callbacks callbacks, bool trial_once = false,
                  bool segmented_input = false, bool scoped_barge_control = false,
                  bool fresh_transport_control = false);
  ~Client();

  bool Start();
  void Stop();
  bool StartSession(std::chrono::milliseconds timeout = std::chrono::seconds(10),
                    const std::string& instructions_override = "",
                    const IdentitySessionOrigin& identity_origin = {});
  // Zero timeout is deliberate replacement: send close, release/join socket, no protocol ACK wait.
  bool FinishSession(std::chrono::milliseconds timeout = std::chrono::seconds(2));
  bool EnqueueAudio(const std::vector<uint8_t>& chunk);
  bool EnqueueAudio(std::vector<uint8_t>&& chunk);
  // For native VPM speech segments: send a turn boundary only after all
  // preceding audio frames have been sent in order.
  bool EndInputTurn();
  bool CommitUserTurn();
  bool SendSayHello(const std::string& override_text = "");
  void SetPlaybackActive(bool active, bool interrupted = false);
  void RecoverStalledPlayback();
  bool InterruptResponse();
  // Experimental control foundation only. No CLI/config switch or App detector.
  // Set context before StartSession; a context change invalidates current tokens.
  void SetBargeContext(uint64_t epoch, uint64_t capture);
  BargeInController::Token BargeToken();
  bool SetPlaybackActiveBound(const BargeInController::Token& token, bool active,
                              bool interrupted = false);
  // Local-only boundary grant; no existing cloud correlation is implied.
  bool AuthorizeBargeResponse(const BargeInController::Token& token,
                             const std::string& response_id);
  bool EnqueueAudioBound(const BargeInController::Token& token, std::vector<uint8_t> chunk);
  // stop_playback must synchronously confirm local stop and must not call Client.
  bool ConfirmBargeIn(const BargeInController::Token& token,
                      const BargeInController::Eligibility& eligibility,
                      const std::function<bool()>& stop_playback);
  // Independent experimental policy, default off and not wired to App.
  // Prepare only AFTER RetireFreshTransport joined the previous socket and
  // synchronously stopped its Player. An owner thread calls retire, never a WS
  // callback or a Player stop callback. No cancel ACK/response.created is used.
  bool PrepareFreshTransport(const TransportEnvelope& envelope);
  // Transition from a quiescent legacy or fresh Client: AFTER its Stop/join,
  // BEFORE this successor Start/Prepare. Same ledger/limit/trial policy required.
  // Retired observations/UNKNOWN/dedup are shared with the quiescent successor.
  bool InheritRetiredUsage(Client& retired);
  bool GrantFreshInput(const ResumePermit& permit, const std::string& session_id,
                       uint64_t capture, const FreshTransportOwner::Proof& proof);
  bool RetireFreshTransport(const TransportEnvelope& envelope,
                            const std::function<bool()>& stop_playback);
  bool EnqueueFreshAudio(const TransportEnvelope& envelope, std::vector<uint8_t> chunk);
  struct CapturedInput {
    audio::shadow::Context origin;
    uint64_t first_sample{0}, first_read_ns{0}, delivered_ns{0};
    uint64_t transfer_input{0}; // Destination owner input, never a provider ID.
  };
  bool EnqueueFreshCapturedAudio(const TransportEnvelope&,std::vector<uint8_t>,const CapturedInput&);
  // Owner/WS-safe: close input admission and discard unsent PCM; only sender
  // writes mute. Already admitted socket writes cannot be recalled. No ACK wait.
  bool PauseFreshInput(const TransportEnvelope& origin,const std::string& reason);
  // Actual VAD endpoint after all captured PCM has been transferred. Drain only
  // accepted PCM, at most one padded tail, one commit then mute. Not after Pause.
  bool EndFreshInput(const TransportEnvelope& origin);
  // App calls only after valid audio.done AND actual Player queue/device drain.
  bool ConfirmFreshPlaybackDrained(const TransportEnvelope& origin);
  // Real parser/effects entry also available for already captured async tasks.
  void HandleTextBound(const TransportEnvelope& envelope, const std::string& text);
  size_t UnsettledFreshUsage(); // Pending billing observations, not output authority.
  struct FreshUsageReceipt {
    TransportEnvelope origin;
    std::string response_id,question_id,source_type,event_id,response_path,question_path,conflict;
    std::string total_event_id,conflicting_response_id,conflicting_question_id,conflict_source_type,conflict_event_id;
    bool retired{false},expected{false},seen_done{false},settled{false};
    bool missing_total{false},ledger_failed{false},covered_by_anonymous{false};
    std::optional<uint64_t> total_tokens,conflicting_total;
  };
  struct FreshUsageSnapshot {
    std::vector<FreshUsageReceipt> receipts;size_t unsettled{0};
    bool storage_unknown{false},attribution_unknown{false};size_t missing_totals{0};
    uint64_t evicted_transport_frontier{0};
  };
  FreshUsageSnapshot InspectFreshUsage(); // Bounded IDs/numbers/provenance only, no user text.

 private:
  o2::Session o2_session_;
  std::mutex o2_mu_;
  bool o2_connection_ready_{false};
  bool OpenConnection(std::chrono::milliseconds timeout);
  void CloseSocket();
  void HandleText(const std::string& text, const IdentitySessionOrigin& origin = {});
  void HandleConnectionLost(const char* reason, bool only_if_active = false);
  void HandleFreshConnectionLost(const TransportEnvelope& envelope, const char* reason);
  void RequestCleanup();
  void JoinCleanup();
  void NotifyClosed(const char* reason);
  bool SendText(const std::string& text);
  bool SendTextLocked(const std::string& text);
  bool SendAudioFrame(const std::vector<uint8_t>& pcm,
                      const BargeInController::Token& token = {});
  bool SendFreshAudioFrame(const std::vector<uint8_t>& pcm,const TransportEnvelope& envelope);
  bool SendFreshCapturedFrame(const std::vector<uint8_t>&,const TransportEnvelope&,const CapturedInput&);
  bool SendFreshControl(const TransportEnvelope&,int control);
  void QueueFreshMuteLocked(const TransportEnvelope&);
  void NoteFreshUsageInput(const TransportEnvelope&);
  size_t PendingUsageLocked() const;
  bool PruneSettledUsageLocked();
  void AccountRetiredUsage(const TransportEnvelope& envelope,const std::string& text);
  void SendLoop();
  bool SuppressInput() const;

  IdentitySessionOrigin identity_origin_;
  config::Config cfg_;
  Callbacks callbacks_;
  const bool trial_once_{false};
  const bool segmented_input_{false};
  const bool scoped_barge_control_{false};
  const bool fresh_transport_control_{false};
  FreshTransportOwner fresh_owner_;
  TransportEnvelope fresh_envelope_;
  bool fresh_retired_{true},fresh_connection_attempted_{false};
  bool captured_input_bound_{false},fresh_mute_sent_{false};
  CapturedInput last_captured_input_;
  struct UsageTransport {
    TransportEnvelope envelope;bool retired{false};
    FreshUsageReceipt anonymous;std::vector<FreshUsageReceipt> responses;
    std::string attribution_conflict;
  };
  struct RetiredBook {
    std::mutex mutex;std::deque<UsageTransport> transports;
    uint64_t evicted_transport_frontier{0},evicted_input_frontier{0};
  };
  std::shared_ptr<RetiredBook> retired_usage_=std::make_shared<RetiredBook>();
  // At most128 transport receipts and128 provider response receipts, including
  // settled dedup. Only wholly settled retired transports may leave retention;
  // late done beyond retention is UNKNOWN, never re-debited or output authority.
  BargeInController barge_;
  uint64_t barge_epoch_{0}, capture_generation_{0};
  std::atomic<uint64_t> socket_generation_{0};
  // Serialize scoped output callbacks against cancellation. Recursive because
  // existing callbacks may synchronously query this Client.
  std::recursive_mutex callback_fence_;
  DailyUsage usage_;
  struct UsageUnknown {
    std::shared_ptr<std::atomic<bool>> value=std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<size_t>> missing=std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<bool>> attribution=std::make_shared<std::atomic<bool>>(false);
    bool load() const { return value->load() || missing->load()!=0 || attribution->load(); }
    void store(bool v) { value->store(v); }
  } usage_unknown_;
  std::atomic<int64_t> playback_hold_until_ns_{0};
  std::atomic<bool> connection_attempted_{false};
  std::atomic<bool> running_{false};
  std::thread sender_thread_;

  std::mutex conn_mu_;
  std::condition_variable conn_cv_;
  std::unique_ptr<ix::WebSocket> ws_;
  bool connected_{false};
  bool connection_failed_{false};

  std::mutex write_mu_;
  std::mutex state_mu_;
  std::condition_variable state_cv_;
  duplex::Session session_;
  bool ai_speaking_{false};
  size_t output_audio_chunks_{0};
  size_t output_audio_bytes_{0};
  audio::OutputFormat output_format_;
  size_t format_probe_blocks_{0};
  bool first_output_delta_logged_{false};
  int raw_dump_fd_{-1};
  size_t raw_dump_bytes_{0};
  bool close_notified_{false};
  std::mutex cleanup_mu_;
  std::thread cleanup_thread_;
  bool cleanup_requested_{false};

  std::mutex audio_mu_;
  std::condition_variable audio_cv_;
  struct AudioItem {
    std::vector<uint8_t> pcm;
    bool end_turn{false};
    BargeInController::Token token;
    TransportEnvelope envelope;
    CapturedInput captured;
    int fresh_control{0}; // 1 mute, 2 end input, 3 unmute; sender is sole writer.
    bool smart_commit{false};
  };
  std::deque<AudioItem> audio_queue_;
  size_t audio_queue_bytes_{0};
  bool user_turn_open_{false}, user_turn_committed_{false}; // state_mu_
};

}  // namespace xiaoai_plus::realtime
