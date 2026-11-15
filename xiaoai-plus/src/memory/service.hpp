#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "config/config.hpp"
#include "memory/core.hpp"
#include "realtime/transport_owner.hpp"

namespace xiaoai_plus::memory {
struct Reply { int status{0}; Json body=Json::object(); };
using Transport = std::function<Reply(const std::string&,const Json&)>;
Transport HttpsTransport(const config::Memory& cfg, const std::string& ca_file);
struct Action {
  uint64_t epoch{0};
  bool restart{true};
  std::string instructions, greeting;
  bool identity_pending{false};
  bool exit{false};
  uint64_t identity_revision{0};
  bool after_reply{false};
  int start_failures{0};
};
struct ResumeIdentity {
  uint64_t epoch{0}, revision{0}, cache_revision{0};
  int64_t generation{-1}, profile_version{-1}, expires_at{0};
  std::string kind, child_id;
  bool operator==(const ResumeIdentity& b) const {
    return epoch==b.epoch && revision==b.revision && cache_revision==b.cache_revision &&
      generation==b.generation && profile_version==b.profile_version && expires_at==b.expires_at &&
      kind==b.kind && child_id==b.child_id;
  }
};
struct ResumeResult {
  bool approved{false};
  std::string reason;
  ResumeIdentity identity;
  realtime::TransportEnvelope envelope;
  realtime::ResumePermit permit;
  std::string instructions; // Only bounded, redacted instructions; no raw profile/snapshot.
};
// All HTTP, redaction and disk I/O happen here, never on the capture/network callbacks.
class Service {
 public:
  Service(config::Memory cfg, Transport transport, std::function<void(Action)> action);
  ~Service();
  void Start();
  void Stop();
  bool Begin(uint64_t epoch);
  bool Resume(uint64_t epoch);
  bool Prefetch();
  uint64_t cache_revision() const { return cache_revision_.load(); }
  uint64_t snapshot_receipts() const { return snapshot_receipts_.load(); }
  bool Activate(uint64_t epoch);
  bool ActivateIdentitySession(const realtime::IdentitySessionOrigin&,std::function<void(ResumeResult)>);
  bool IdentitySessionEvent(const realtime::IdentitySessionOrigin&,const realtime::duplex::Event&);
  bool Event(uint64_t epoch, const realtime::duplex::Event& event);
  bool End(uint64_t epoch, bool complete = true);
  bool Interrupt(uint64_t epoch);
  // Async replies run on the Service worker. Never block it waiting for Client.
  bool InspectTransportResume(uint64_t epoch, std::function<void(ResumeResult)> reply);
  bool PrepareTransportResume(const ResumeIdentity& expected, uint64_t transport,
                              uint64_t capture, uint64_t input,
                              std::function<void(ResumeResult)> reply);
  bool ActivateTransportResume(const realtime::TransportEnvelope& envelope,
                               std::function<void(ResumeResult)> reply);
  bool Event(const realtime::TransportEnvelope& envelope, const realtime::duplex::Event& event);
  bool RetireTransportResume(const realtime::TransportEnvelope& envelope);
  bool PlaybackDrained(const realtime::TransportEnvelope& envelope);
  bool End(const realtime::TransportEnvelope& envelope, bool complete = true);
  bool Interrupt(const realtime::TransportEnvelope& envelope);
  std::string status() const;
 private:
  struct Command {
    int kind;uint64_t epoch;realtime::duplex::Event event;bool complete{true};
    ResumeIdentity expected;
    realtime::IdentitySessionOrigin identity_origin;
    realtime::TransportEnvelope envelope;
    uint64_t transport{0}, capture{0}, input{0};
    std::function<void(ResumeResult)> reply;
  };
  bool Push(Command command);
  void Loop();
  void Process(const Command& command);
  bool Sync();
  void EmitAction(Action action);
  void ResolveIdentity(const std::string& text, Candidate candidate = {});
  std::string ConversationInstructions(bool short_resume = false) const;
  void FinishClassification(const std::string& result);
  realtime::IdentitySessionOrigin legacy_identity_origin_;
  uint64_t last_legacy_session_{0}, identity_session_expected_revision_{0};
  bool ApplySnapshot(Reply reply);
  void NetworkLoop();
  void Schedule(int kind, Json body = Json());
  void CompleteNetwork(bool storage_ok);
  void Flush();
  void PersistActive();
  void PersistPending();
  void AddPending(const Json& session);
  size_t PendingBytes() const;
  void Seal(bool complete);
  Json Child(const std::string& id) const;
  void SetStatus(std::string value);
  ResumeResult ResumeState(uint64_t epoch) const;
  bool ActiveResumeValid() const;
  void AlignActiveRecorder();
  void RevokeResume();
  config::Memory cfg_;
  Transport transport_;
  std::function<void(Action)> action_;
  std::atomic<bool> running_{false}, overflow_{false};
  std::thread thread_, network_thread_;
  struct NetworkJob { int kind; Json body; uint64_t epoch, identity_revision; };
  struct NetworkResult { int kind; Json request; Reply reply; uint64_t epoch, identity_revision; };
  std::mutex network_mu_;
  std::condition_variable network_cv_;
  std::deque<NetworkJob> network_jobs_;
  std::deque<NetworkResult> network_results_;
  bool sync_inflight_{false}, upload_inflight_{false}, ack_inflight_{false};
  std::atomic<uint64_t> cache_revision_{0};
  std::atomic<uint64_t> snapshot_receipts_{0};
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Command> commands_;
  std::string status_{"ok"};
  Identity identity_;
  bool identity_asked_{false};
  Json snapshot_=Json::object(), pending_=Json::array(), active_=Json::object();
  Transcript transcript_;
  std::string continuation_;
  std::chrono::steady_clock::time_point ended_at_{};
  std::unordered_set<std::string> identity_events_;
  uint64_t epoch_{0};
  uint64_t identity_revision_{1}, resume_sequence_{0}, last_transport_{0}, last_input_{0};
  bool transport_mode_{false}, resume_granted_{false};
  realtime::TransportEnvelope resume_envelope_;
  ResumeIdentity resume_identity_;
  Json resume_snapshot_=Json::object(); // Private immutable authorization facts for this input.
  std::string resume_instructions_;
  std::shared_ptr<std::atomic<bool>> resume_live_;
  bool recording_{false}, transitioning_{false}, conversational_{false};
  std::chrono::steady_clock::time_point next_sync_{}, next_retry_{};
  int backoff_{5};
  Json classification_=Json::object();
  uint64_t classification_seq_{0};
  std::chrono::steady_clock::time_point classification_deadline_{};
};
}  // namespace xiaoai_plus::memory
