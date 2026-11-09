#include "wakeup/local_listener.hpp"
#include "wakeup/exit_echo_guard.hpp"

#include "common/log.hpp"

namespace xiaoai_plus::wakeup {

namespace {
const auto kLog = xiaoai_plus::GetLogger("kws");
}  // namespace

LocalListener::LocalListener(Config cfg)
    : cfg_(std::move(cfg)),
      decider_(cfg_.trigger && cfg_.trigger->Allows("豆包") &&
               cfg_.trigger->Allows("豆包豆包")) {
  if(cfg_.kws_engine)cfg_.kws_engine->SetExitWordsEnabled(false);
}

bool LocalListener::UpdateExitWordsLocked(std::chrono::steady_clock::time_point now) {
  const bool active = cfg_.gate->step() == Step::kActive;
  const bool speaking = active && cfg_.gate->IsAiSpeaking();
  const bool allowed = !cfg_.exit_allowed || cfg_.exit_allowed();
  if (active && (!was_active_ || (allowed && !was_exit_allowed_))) active_since_ = now;
  was_exit_allowed_ = allowed;
  if (was_ai_speaking_ && !speaking) ai_quiet_since_ = now;
  was_active_ = active;
  was_ai_speaking_ = speaking;
  const bool enabled = active && allowed && !speaking &&
                       now - active_since_ >= cfg_.exit_after_wake &&
                       now - ai_quiet_since_ >= cfg_.exit_after_ai_speech;
  if (enabled != exit_enabled_ || active != strong_enabled_) {
    exit_enabled_ = enabled;strong_enabled_=active;
    cfg_.kws_engine->SetExitWordsEnabled(active,enabled);
  }
  return enabled;
}

LocalListener::Decision LocalListener::AnalyzePcm(const std::vector<uint8_t>& chunk,
    std::chrono::steady_clock::time_point birth) {
  if (chunk.empty() || !cfg_.gate || !cfg_.trigger || !cfg_.kws_engine) return {};
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_) return {};
  const bool active = cfg_.gate->step() == Step::kActive;
  const bool allow_wake = !active ||
      (cfg_.allow_playback_interrupt && cfg_.gate->IsAiSpeaking());
  const auto before = std::chrono::steady_clock::now();
  if (cfg_.bounded_decisions && prefix_birth_.time_since_epoch().count() &&
      before > prefix_due_ + std::chrono::milliseconds(500)) {
    decider_.Reset(); prefix_birth_ = {};
  }
  if (active) {decider_.Reset();prefix_birth_ = {};}
  const bool exit_enabled = UpdateExitWordsLocked(before);
  auto hit = cfg_.kws_engine->AcceptPcm16(chunk.data(), chunk.size(), cfg_.sample_rate,
                                        cfg_.channels, cfg_.bit_depth);
  const auto now = std::chrono::steady_clock::now();
  std::optional<std::string> decided;
  bool end_session = false;
  auto decision_birth = birth;
  bool held_prefix = false;
  auto due = prefix_due_;
  if (cfg_.bounded_decisions && (now < birth || now-birth > std::chrono::milliseconds(500))) {
    decider_.Reset();prefix_birth_={};prefix_due_={};return {};
  }
  if (hit) {
    const bool strong = !StrongExitWord(hit->keyword).empty();
    if (hit->keyword == "结束" || strong) {
      decider_.Reset();prefix_birth_ = {};
      end_session = active && (strong || exit_enabled) && static_cast<bool>(cfg_.on_end);
      const bool playing=cfg_.gate->IsAiSpeaking();
      if(hit->score<cfg_.exit_playback_threshold)end_session=false;
      const bool playback_or_tail = playing || now-ai_quiet_since_ < cfg_.exit_after_ai_speech;
      if (end_session && strong && playback_or_tail && cfg_.ai_text_contains &&
          cfg_.ai_text_contains(hit->keyword)) end_session = false;
      kLog->info("exit keyword={} score={:.6f} playing={} threshold={:.2f} accepted={}",
        strong?StrongExitWord(hit->keyword):"再见",hit->score,playing,
        cfg_.exit_playback_threshold,end_session);
    } else if (allow_wake && cfg_.trigger->Allows(hit->keyword)) {
      if (prefix_birth_.time_since_epoch().count()) {decision_birth = prefix_birth_;held_prefix = true;}
      decided = decider_.OnHit(hit->keyword, now);
      if (!decided && !prefix_birth_.time_since_epoch().count()) {prefix_birth_ = birth;prefix_due_ = now + std::chrono::milliseconds(1100);due = prefix_due_;}
      if (decided) prefix_birth_ = {};
    }
  } else if (allow_wake) {
    if (prefix_birth_.time_since_epoch().count()) {decision_birth = prefix_birth_;held_prefix = true;}
    decided = decider_.Tick(now);
    if (decided) prefix_birth_ = {};
  }
  if (!decided && !end_session) return {};
  if (cfg_.bounded_decisions && ((!held_prefix && (decision_birth.time_since_epoch().count()==0 ||
      now < decision_birth || now-decision_birth > std::chrono::milliseconds(500))) ||
      (held_prefix && now > due + std::chrono::milliseconds(500)))) {
    decider_.Reset();prefix_birth_ = {};return {};
  }
  if (decided && last_trigger_.time_since_epoch().count() > 0 &&
      now-last_trigger_ < std::chrono::milliseconds(cfg_.min_trigger_interval_ms)) return {};
  return {decided.value_or(""),end_session,decision_birth,held_prefix,due};
}
void LocalListener::AcceptPcm(const std::vector<uint8_t>& chunk) {
  const auto result = AnalyzePcm(chunk,std::chrono::steady_clock::now());
  if (!result) return;
  if (result.end_session) {
    if (cfg_.gate->step()==Step::kActive && cfg_.on_end) cfg_.on_end();
  } else if (cfg_.trigger->FireFromText(result.keyword)) {
    std::lock_guard<std::mutex> lock(mu_);last_trigger_ = std::chrono::steady_clock::now();
  }
}
void LocalListener::NoteAccepted() {
  std::lock_guard<std::mutex> lock(mu_);last_trigger_=std::chrono::steady_clock::now();
}
void LocalListener::Reset() {
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_) return;
  decider_.Reset();prefix_birth_ = {};
  was_active_=false;was_ai_speaking_=false;was_exit_allowed_=false;
  exit_enabled_=false;strong_enabled_=false;
  active_since_={};ai_quiet_since_={};
  cfg_.kws_engine->Reset();cfg_.kws_engine->SetExitWordsEnabled(false,false);
}

void LocalListener::Close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_) return;
  closed_ = true;
  decider_.Reset();
  if (cfg_.kws_engine) {
    cfg_.kws_engine->Reset();
  }
}

}  // namespace xiaoai_plus::wakeup
