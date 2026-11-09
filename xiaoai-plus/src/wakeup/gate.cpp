#include "wakeup/gate.hpp"

#include <algorithm>

#include "common/log.hpp"

namespace xiaoai_plus::wakeup {

namespace {
const auto kLog = xiaoai_plus::GetLogger("gate");
}  // namespace

Gate::Gate(std::chrono::milliseconds timeout, Hooks hooks,
           std::chrono::milliseconds absolute_timeout,
           std::chrono::milliseconds speaking_stall,
           std::chrono::milliseconds user_turn_limit)
    : timeout_(timeout.count() > 0 ? timeout : std::chrono::seconds(60)),
      absolute_timeout_(absolute_timeout),
      speaking_stall_(speaking_stall),
      user_turn_limit_(user_turn_limit),
      hooks_(std::move(hooks)) {
  timeout_thread_ = std::thread([this]() {
    try { TimeoutLoop(); }
    catch (const std::exception& e) {
      kLog->error("gate thread exception: {}", e.what());
      if (hooks_.on_fatal) hooks_.on_fatal();
    } catch (...) {
      kLog->error("gate thread unknown exception");
      if (hooks_.on_fatal) hooks_.on_fatal();
    }
  });
}

Gate::~Gate() { Close(); }

Step Gate::step() const {
  std::lock_guard<std::mutex> lock(mu_);
  return step_;
}

bool Gate::IsAiSpeaking() const {
  std::lock_guard<std::mutex> lock(mu_);
  return step_ == Step::kActive && is_ai_speaking_;
}

Gate::WakeCommit Gate::CommitWakeup(const std::string& keyword) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_ || cleanup_revision_ || (step_ == Step::kActive && !is_ai_speaking_)) return {};
  WakeCommit result{++wake_revision_, step_ == Step::kActive, "local_kws: " + keyword};
  if (step_ == Step::kIdle) {
    user_turn_open_ = false;
    step_ = Step::kActive;
    absolute_deadline_ = std::chrono::steady_clock::now() + absolute_timeout_;
  }
  is_ai_speaking_ = false;
  RefreshTimeoutLocked();
  return result;
}
uint64_t Gate::Revision() const {
  std::lock_guard<std::mutex> lock(mu_); return wake_revision_;
}
bool Gate::IsWakeCurrent(const WakeCommit& result) const {
  std::lock_guard<std::mutex> lock(mu_);
  return result && !closed_ && step_ == Step::kActive && result.revision == wake_revision_;
}
void Gate::CancelWakeup(const WakeCommit& result) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!result || closed_ || wake_revision_ != result.revision) return;
  ++wake_revision_;
  if (result.interrupt) is_ai_speaking_ = true;
  else {step_ = Step::kIdle;is_ai_speaking_ = false;}
  ++timeout_epoch_; timeout_cv_.notify_all();
}
bool Gate::TryWakeup(const std::string& keyword) {
  const auto result = CommitWakeup(keyword);
  if (!result) return false;
  if (result.interrupt) {if (hooks_.on_interrupt) hooks_.on_interrupt(result.reason);}
  else if (hooks_.on_arm) hooks_.on_arm(result.reason);
  return true;
}

void Gate::Activate(const std::string& reason) {
  bool was_idle = false;
  std::function<void(const std::string&)> on_arm;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if(closed_ || cleanup_revision_)return;
    was_idle = (step_ == Step::kIdle);
    if (was_idle) ++wake_revision_;
    step_ = Step::kActive;
    if (was_idle) {
      user_turn_open_ = false;
      is_ai_speaking_ = false;
      absolute_deadline_ = std::chrono::steady_clock::now() + absolute_timeout_;
    }
    RefreshTimeoutLocked();
    on_arm = hooks_.on_arm;
  }
  if (was_idle && on_arm) {
    on_arm(reason);
  }
}

Gate::DisarmCommit Gate::CommitDisarm(const std::string& reason) {
  std::lock_guard<std::mutex> lock(mu_);
  if (closed_ || step_ == Step::kIdle) return {};
  step_=Step::kIdle;is_ai_speaking_=false;
  ++timeout_epoch_;timeout_cv_.notify_all();
  return {++wake_revision_,reason};
}
bool Gate::IsDisarmCurrent(const DisarmCommit& result) const {
  std::lock_guard<std::mutex> lock(mu_);
  return result && !closed_ && step_==Step::kIdle && wake_revision_==result.revision;
}
bool Gate::ClaimDisarmCleanup(const DisarmCommit& result) {
  std::lock_guard<std::mutex> lock(mu_);
  if(!result || closed_ || step_!=Step::kIdle || wake_revision_!=result.revision || cleanup_revision_)return false;
  cleanup_revision_=result.revision;return true;
}
void Gate::ReleaseDisarmCleanup(const DisarmCommit& result) {
  std::lock_guard<std::mutex> lock(mu_);
  if(cleanup_revision_==result.revision)cleanup_revision_=0;
}
void Gate::DispatchDisarm(const DisarmCommit& result) {
  if(!IsDisarmCurrent(result))return;
  if(hooks_.after_disarm_scoped)hooks_.after_disarm_scoped(result.reason,result.revision);
  else if(hooks_.after_disarm)hooks_.after_disarm(result.reason);
}
void Gate::Disarm(const std::string& reason) {
  DispatchDisarm(CommitDisarm(reason));
}

void Gate::RefreshTimeout() {
  std::lock_guard<std::mutex> lock(mu_);
  if (step_ == Step::kActive) {
    RefreshTimeoutLocked();
  }
}

void Gate::SetUserTurnOpen(bool open) {
  std::lock_guard<std::mutex> lock(mu_);
  if (open) {
    if (step_ != Step::kActive) return;
    user_turn_open_ = true;
    user_turn_deadline_ = std::chrono::steady_clock::now() + user_turn_limit_;
    ++timeout_epoch_;
    timeout_cv_.notify_all();
  } else if (user_turn_open_) {
    user_turn_open_ = false;
    RefreshTimeoutLocked();
  }
}

void Gate::RefreshTimeoutLocked() {
  timeout_deadline_ = std::chrono::steady_clock::now() + timeout_;
  ++timeout_epoch_;
  timeout_cv_.notify_all();
}

void Gate::SetAiSpeaking(bool is_speaking) {
  std::lock_guard<std::mutex> lock(mu_);
  if (is_speaking) user_turn_open_ = false;
  if (is_ai_speaking_ == is_speaking) {
    return;
  }
  is_ai_speaking_ = is_speaking;
  if (step_ != Step::kActive) {
    return;
  }

  if (is_ai_speaking_) {
    speaking_progress_deadline_ = std::chrono::steady_clock::now() + speaking_stall_;
    // Pause timeout while AI is speaking.
    ++timeout_epoch_;
    timeout_cv_.notify_all();
  } else {
    // Resume timeout countdown from now when AI speech ends.
    RefreshTimeoutLocked();
  }
}

void Gate::NoteAiProgress() {
  std::lock_guard<std::mutex> lock(mu_);
  if (step_ == Step::kActive && is_ai_speaking_) {
    speaking_progress_deadline_ = std::chrono::steady_clock::now() + speaking_stall_;
    ++timeout_epoch_;
    timeout_cv_.notify_all();
  }
}

void Gate::Close() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (closed_) {
      return;
    }
    closed_ = true;
    ++wake_revision_;
    ++timeout_epoch_;
  }
  timeout_cv_.notify_all();
  if (timeout_thread_.joinable()) {
    timeout_thread_.join();
  }
}

void Gate::TimeoutLoop() {
  std::unique_lock<std::mutex> lock(mu_);
  while (!closed_) {
    if (step_ != Step::kActive) {
      timeout_cv_.wait(lock, [this]() { return closed_ || step_ == Step::kActive; });
      continue;
    }

    const auto absolute_deadline = absolute_deadline_;
    const bool has_absolute_limit = absolute_timeout_.count() > 0;
    if (has_absolute_limit && std::chrono::steady_clock::now() >= absolute_deadline) {
      ++wake_revision_;
    step_ = Step::kIdle;
      is_ai_speaking_ = false;
      ++timeout_epoch_;
      const auto disarm_revision = wake_revision_;
      lock.unlock();
      kLog->warn("gate absolute session limit -> idle");
      DispatchDisarm({disarm_revision,"absolute_timeout"});
      lock.lock();
      continue;
    }

    if (is_ai_speaking_) {
      const auto epoch = timeout_epoch_;
      const auto deadline = has_absolute_limit
          ? std::min(absolute_deadline, speaking_progress_deadline_)
          : speaking_progress_deadline_;
      if (!timeout_cv_.wait_until(lock, deadline, [this, epoch]() {
            return closed_ || step_ != Step::kActive || timeout_epoch_ != epoch || !is_ai_speaking_;
          }) && std::chrono::steady_clock::now() >= speaking_progress_deadline_ &&
          (!has_absolute_limit || std::chrono::steady_clock::now() < absolute_deadline)) {
        is_ai_speaking_ = false;
        RefreshTimeoutLocked();
        auto recover = hooks_.on_speaking_stall;
        lock.unlock();
        kLog->error("AI playback stalled; restoring input and idle timer");
        if (recover) recover();
        lock.lock();
      }
      continue;
    }

    if (user_turn_open_) {
      const auto epoch = timeout_epoch_;
      const auto deadline = has_absolute_limit
          ? std::min(user_turn_deadline_, absolute_deadline) : user_turn_deadline_;
      if (!timeout_cv_.wait_until(lock, deadline, [this, epoch]() {
            return closed_ || step_ != Step::kActive || timeout_epoch_ != epoch ||
                   !user_turn_open_ || is_ai_speaking_;
          }) && std::chrono::steady_clock::now() >= user_turn_deadline_) {
        user_turn_open_ = false;
        timeout_deadline_ = std::chrono::steady_clock::now();
        ++timeout_epoch_;
        kLog->warn("user transcription limit reached; resuming timeout exit");
      }
      continue;
    }

    const auto epoch = timeout_epoch_;
    const auto deadline = has_absolute_limit ? std::min(timeout_deadline_, absolute_deadline)
                                              : timeout_deadline_;
    if (timeout_cv_.wait_until(lock, deadline, [this, epoch]() {
          return closed_ || step_ != Step::kActive || timeout_epoch_ != epoch || is_ai_speaking_;
        })) {
      continue;
    }

    ++wake_revision_;
    step_ = Step::kIdle;
    is_ai_speaking_ = false;
    ++timeout_epoch_;
    const auto disarm_revision = wake_revision_;
    const bool absolute_expired = has_absolute_limit &&
                                  std::chrono::steady_clock::now() >= absolute_deadline;
    lock.unlock();
    kLog->info("gate {} -> idle", absolute_expired ? "absolute_timeout" : "timeout");
    DispatchDisarm({disarm_revision,absolute_expired ? "absolute_timeout" : "timeout"});
    lock.lock();
  }
}

}  // namespace xiaoai_plus::wakeup
