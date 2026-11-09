#pragma once
#include <cstdint>
#include <ctime>
#include <atomic>
#include <memory>
#include <string>
#include <tuple>
#include "realtime/duplex_protocol.hpp"

namespace xiaoai_plus::memory { class Service; }
namespace xiaoai_plus::realtime {
// Local provenance, never a provider ID. Copy into the transport callback and
// every downstream task at registration time; there are no mutators.
class TransportEnvelope {
 public:
  TransportEnvelope() = default;
  TransportEnvelope(uint64_t epoch, uint64_t identity, int64_t snapshot,
                    int64_t profile, uint64_t lease, uint64_t transport,
                    uint64_t capture, uint64_t input)
      : epoch_(epoch), identity_(identity), snapshot_(snapshot), profile_(profile),
        lease_(lease), transport_(transport), capture_(capture), input_(input) {}
  uint64_t epoch() const { return epoch_; }
  uint64_t identity() const { return identity_; }
  int64_t snapshot() const { return snapshot_; }
  int64_t profile() const { return profile_; }
  uint64_t lease() const { return lease_; }
  uint64_t transport() const { return transport_; }
  uint64_t capture() const { return capture_; }
  uint64_t input() const { return input_; }
  bool valid() const { return epoch_ && identity_ && snapshot_ >= 0 &&
                             lease_ && transport_ && capture_ && input_; }
  bool operator==(const TransportEnvelope& b) const { return key() == b.key(); }
  bool operator!=(const TransportEnvelope& b) const { return !(*this == b); }
 private:
  std::tuple<uint64_t,uint64_t,int64_t,int64_t,uint64_t,uint64_t,uint64_t,uint64_t> key() const {
    return std::make_tuple(epoch_,identity_,snapshot_,profile_,lease_,transport_,capture_,input_);
  }
  uint64_t epoch_{0}, identity_{0};
  int64_t snapshot_{-1}, profile_{-1};
  uint64_t lease_{0}, transport_{0}, capture_{0}, input_{0};
};
// Issued only by the real Service worker after it rechecks the prepared lease.
// Immutable origin for the identity-session legacy WS created AFTER owner retirement/join.
class IdentitySessionOrigin {
 public:
  IdentitySessionOrigin() = default;
  IdentitySessionOrigin(uint64_t epoch,uint64_t revision,uint64_t session)
    :epoch_(epoch),revision_(revision),session_(session) {}
  uint64_t epoch() const {return epoch_;}
  uint64_t revision() const {return revision_;}
  uint64_t session() const {return session_;}
  bool valid() const {return epoch_ && revision_ && session_;}
  bool operator==(const IdentitySessionOrigin& b) const {
    return epoch_==b.epoch_ && revision_==b.revision_ && session_==b.session_;
  }
 private:
  uint64_t epoch_{0},revision_{0},session_{0};
};
class ResumePermit {
 public:
  ResumePermit() = default;
  const TransportEnvelope& envelope() const { return envelope_; }
  bool valid() const { return envelope_.valid() && live_ && live_->load() &&
    expires_at_>std::time(nullptr); }
 private:
  friend class xiaoai_plus::memory::Service;
  ResumePermit(TransportEnvelope envelope,std::shared_ptr<std::atomic<bool>> live,int64_t expires_at)
    : envelope_(envelope),live_(std::move(live)),expires_at_(expires_at) {}
  TransportEnvelope envelope_;
  int64_t expires_at_{0}; // Immutable original authorization expiry, never refreshed by fetch.
  std::shared_ptr<std::atomic<bool>> live_; // Service-only revocation, token stays immutable.
};
class FreshTransportOwner {
 public:
  struct Proof {
    bool old_transport_joined{false}, playback_stopped{false}, context_ready{false};
    bool capture_continuous{false}, exclusive_input{false}, native_blocked{true};
  };
  explicit FreshTransportOwner(bool enabled = false) : enabled_(enabled) {}
  bool Prepare(const TransportEnvelope& e) {
    if(!enabled_ || current_.valid() || !e.valid() || e.transport()<=last_transport_ || e.input()<=last_input_)return false;
    current_=e;last_transport_=e.transport();last_input_=e.input();
    session_.clear();item_.clear();question_.clear();response_.clear();grant_permit_={};granted_=false;done_=false;has_audio_=false;audio_done_=false;playback_drained_=false;input_=InputState::kAwaitingGrant;return true;
  }
  bool Current(const TransportEnvelope& e) const { return enabled_ && e.valid() && e==current_; }
  bool SessionCreated(const TransportEnvelope& e,const std::string& id) {
    if(!Current(e) || !session_.empty() || id.empty() || id.size()>128)return false;
    session_=id;return true;
  }
  bool Grant(const ResumePermit& permit,const std::string& session,uint64_t capture,const Proof& proof) {
    if(!permit.valid() || !Current(permit.envelope()) || granted_ || done_ || session_.empty() || session!=session_ ||
       capture!=current_.capture() || !proof.old_transport_joined || !proof.playback_stopped ||
       !proof.context_ready || !proof.capture_continuous || !proof.exclusive_input || proof.native_blocked)return false;
    grant_permit_=permit;granted_=true;input_=InputState::kListening;return true;
  }
  bool Lease(const TransportEnvelope& e) const {
    return Current(e) && granted_ && grant_permit_.valid() && !playback_drained_;
  }
  // Enqueue closes immediately; Ending permits only previously accepted PCM.
  bool Input(const TransportEnvelope& e) const { return Lease(e) && input_==InputState::kListening; }
  bool SendInput(const TransportEnvelope& e) const {
    return Lease(e) && (input_==InputState::kListening || input_==InputState::kEnding);
  }
  bool NoteAudio(const TransportEnvelope& e) { if(!Input(e))return false;has_audio_=true;return true; }
  bool Pause(const TransportEnvelope& e) {
    if(!Lease(e) || input_==InputState::kMuted)return false;
    input_=InputState::kMuted;return true;
  }
  bool EndInput(const TransportEnvelope& e) {
    if(!Input(e) || !has_audio_)return false;
    input_=InputState::kEnding;return true;
  }
  bool Ending(const TransportEnvelope& e) const { return Lease(e) && input_==InputState::kEnding; }
  bool InputCommitted(const TransportEnvelope& e) {
    if(!Ending(e))return false;input_=InputState::kMuted;return true;
  }
  bool PlaybackDrained(const TransportEnvelope& e) {
    if(!Lease(e) || !audio_done_)return false;
    playback_drained_=true;input_=InputState::kMuted;return true;
  }
  bool Accept(const TransportEnvelope& e,const duplex::Event& event) {
    if(!Lease(e) || event.warning)return false;
    const bool asr=event.type.rfind("conversation.item.input_audio_transcription.",0)==0;
    const bool output=event.type.rfind("response.output_",0)==0;
    if(asr) {
      if(!event.item_field.valid() || !event.event_field.valid())return false;
      if(!Bind(item_,event.item_id)){Retire(e);return false;}
    }else if(output) {
      if(!event.response_field.valid() || !event.question_field.valid() || !event.event_field.valid())return false;
      if(audio_done_ && (event.type=="response.output_audio.delta" || event.type=="response.output_audio.done"))return false;
      // question_id is deliberately NOT compared with ASR.item_id.
      if(!Bind(question_,event.question_id) || !Bind(response_,event.response_id)){Retire(e);return false;}
      if(event.type=="response.output_audio.done")audio_done_=true;
    }else return false; // response.done is billing only, never output completion.
    return true;
  }
  bool Retire(const TransportEnvelope& e) {
    if(!Current(e))return false;
    current_={};grant_permit_={};granted_=false;done_=true;input_=InputState::kMuted;return true;
  }
 private:
  static bool Bind(std::string& known,const std::string& id) { if(known.empty()){known=id;return true;}return known==id; }
  enum class InputState { kAwaitingGrant,kListening,kEnding,kMuted };
  InputState input_{InputState::kAwaitingGrant};
  bool has_audio_{false},audio_done_{false},playback_drained_{false};
  bool enabled_{false}, granted_{false}, done_{false};
  uint64_t last_transport_{0}, last_input_{0};
  TransportEnvelope current_;
  ResumePermit grant_permit_;
  std::string session_,item_,question_,response_;
};
}  // namespace xiaoai_plus::realtime
