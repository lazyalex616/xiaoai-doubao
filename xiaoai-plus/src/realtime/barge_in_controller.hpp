#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>

namespace xiaoai_plus::realtime {

// State-owner component, with no detector, socket or memory-service calls.
// The caller serializes every operation (Client uses its state mutex).
// After cancellation, an externally correlated id grant AND response.created
// are required. A fresh id alone does not prove a new-input response boundary.
// An anonymous canceled/done is deliberately NOT a cancellation acknowledgement.
class BargeInController {
 public:
  struct Token {
    uint64_t epoch{0}, session{0}, response{0}, ingress{0}, playback{0}, capture{0};
    bool operator==(const Token& b) const {
      return epoch == b.epoch && session == b.session && response == b.response &&
             ingress == b.ingress && playback == b.playback && capture == b.capture;
    }
    bool operator!=(const Token& b) const { return !(*this == b); }
  };
  struct Eligibility {
    bool active{false}, ready{false}, identity_switching{false}, native_blocked{false};
    bool capture_continuous{false};
  };
  enum class Created { kAccepted, kDuplicate, kRejected };

  explicit BargeInController(bool enabled = false) : enabled_(enabled) {}
  bool enabled() const { return enabled_; }
  Token token() const { return token_; }
  bool active() const { return active_; }
  bool playing() const { return playing_; }
  const std::string& response_id() const { return response_id_; }

  void BeginSession(uint64_t epoch, uint64_t capture) {
    ++token_.session;
    token_.epoch = epoch;
    token_.capture = capture;
    token_.response = 0;
    ++token_.ingress;
    ++token_.playback;
    active_ = enabled_ && epoch != 0 && capture != 0;
    playing_ = false;
    canceled_ = false;
    cancellation_pending_ = false;
    require_grant_ = false;
    next_response_id_.clear();
    response_id_.clear();
    seen_ids_.clear();
  }
  void Invalidate() {
    active_ = false;
    playing_ = false;
    canceled_ = true;
    cancellation_pending_ = false;
    ++token_.ingress;
    ++token_.playback;
  }
  // Capture discontinuity invalidates all held PCM and synthetic confirmations.
  void ChangeCapture(uint64_t capture) {
    token_.capture = capture;
    Invalidate();
  }
  // The caller must establish the response-to-new-input correlation outside
  // this component. Current wire protocol does not establish that contract;
  // production App does not call this API. Synthetic tests supply a grant.
  bool AuthorizeNextResponse(const Token& input, const std::string& id) {
    if (!AcceptInput(input) || !require_grant_ || !canceled_ || id.empty() ||
        id.size() > 128 || seen_ids_.count(id)) return false;
    next_response_id_ = id;
    return true;
  }
  Created ResponseCreated(const std::string& id) {
    if (!active_ || id.empty() || id.size() > 128) return Created::kRejected;
    if (id == response_id_ && !canceled_) return Created::kDuplicate;
    if (seen_ids_.count(id) || (require_grant_ && id != next_response_id_))
      return Created::kRejected;
    // Bound local state. Reusing an evicted id would defeat a late-output fence.
    if (seen_ids_.size() == 128) { Invalidate(); return Created::kRejected; }
    seen_ids_.insert(id);
    require_grant_ = false;
    next_response_id_.clear();
    response_id_ = id;
    ++token_.response;
    ++token_.playback;
    ++token_.ingress;
    canceled_ = false;
    playing_ = false;
    return Created::kAccepted;
  }
  bool AcceptOutput(const std::string& id) const {
    return active_ && !canceled_ && !id.empty() && id == response_id_ &&
           token_.response != 0;
  }
  bool Audio(const std::string& id) {
    if (!AcceptOutput(id)) return false;
    playing_ = true;
    return true;
  }
  bool AudioDone(const std::string& id) {
    if (!AcceptOutput(id)) return false;
    // Cloud has finished producing audio, but local queued audio may still
    // be playing. Only the playback owner can declare it drained.
    return true;
  }
  bool PlaybackDrained(const Token& token) {
    if (!AcceptInput(token)) return false;
    playing_ = false;
    return true;
  }
  bool ConfirmSpeech(const Token& confirmation, const Eligibility& e) {
    if (!enabled_ || !active_ || !playing_ || canceled_ || confirmation != token_ ||
        !e.active || !e.ready || e.identity_switching || e.native_blocked ||
        !e.capture_continuous) return false;
    canceled_ = true;
    cancellation_pending_ = true;
    require_grant_ = true;
    next_response_id_.clear();
    playing_ = false;
    ++token_.ingress;
    ++token_.playback;
    return true;
  }
  // Stop/cancel/unmute completion is local transport completion, not cloud ACK.
  bool CancellationPending(const Token& token) const {
    return active_ && canceled_ && cancellation_pending_ && token == token_;
  }
  bool CompleteCancellation(const Token& token) {
    if (!CancellationPending(token)) return false;
    cancellation_pending_ = false;
    return true;
  }
  // Exact local ingress token; no assumption about server ASR event attribution.
  bool AcceptInput(const Token& input) const {
    return active_ && !cancellation_pending_ && input == token_;
  }

 private:
  const bool enabled_{false};
  Token token_;
  bool active_{false}, playing_{false}, canceled_{false};
  bool require_grant_{false}, cancellation_pending_{false};
  std::string response_id_, next_response_id_;
  std::unordered_set<std::string> seen_ids_;
};

}  // namespace xiaoai_plus::realtime
