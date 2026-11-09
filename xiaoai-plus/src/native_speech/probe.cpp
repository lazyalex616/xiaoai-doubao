#include "native_speech/probe.hpp"

#include <algorithm>
#include <iostream>

namespace xiaoai_plus::native_speech {

Probe::Probe(Source::Options options) : source_(std::move(options)) {}

bool Probe::Run(std::chrono::seconds maximum, Counts* counts,
                bool require_complete_turn) {
  if (!counts || maximum < std::chrono::seconds(1) ||
      maximum > std::chrono::seconds(30)) return false;
  const auto deadline = std::chrono::steady_clock::now() + maximum;
  Source::Callbacks cb;
  cb.on_turn = [this](uint64_t id, Source::Activation mode) {
    std::lock_guard<std::mutex> lock(mu_);
    if (mode == Source::Activation::kWake) ++counts_.wakes;
    else if (mode == Source::Activation::kFollowUp) ++counts_.followups;
    active_turn_ = id;
    active_turn_pcm_ = false;
    active_turn_wake_ = mode == Source::Activation::kWake;
    cv_.notify_all();
  };
  cb.on_pcm = [this](uint64_t id, const std::vector<uint8_t>& pcm) {
    std::lock_guard<std::mutex> lock(mu_);
    ++counts_.pcm_packets;
    counts_.pcm_bytes += pcm.size();
    if (id == active_turn_ && !pcm.empty()) active_turn_pcm_ = true;
  };
  cb.on_end = [this](uint64_t id) {
    const bool finished = source_.FinishDialog(id);
    std::lock_guard<std::mutex> lock(mu_);
    if (finished) {
      ++counts_.ended;
      if (id == active_turn_ && active_turn_pcm_ && active_turn_wake_)
        ++counts_.complete_turns;
    }
    else failed_ = true;
    done_ = true;
    cv_.notify_all();
  };
  cb.on_cancel = [this](uint64_t id) {
    const bool finished = source_.FinishDialog(id);
    std::lock_guard<std::mutex> lock(mu_);
    if (!finished) failed_ = true;
    done_ = true;
    cv_.notify_all();
  };
  cb.on_fatal = [this](const std::string&) {
    std::lock_guard<std::mutex> lock(mu_);
    failed_ = true;
    done_ = true;
    cv_.notify_all();
  };
  if (!source_.Start(std::move(cb))) {
    counts->registration = source_.diagnostics();
    return false;
  }
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
  const bool registered = remaining.count() > 0 &&
      source_.WaitForRegistered(std::min(remaining, std::chrono::milliseconds(15000)));
  {
    std::lock_guard<std::mutex> lock(mu_);
    counts_.registered = registered;
  }
  if (registered) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    std::cout << "native_probe READY_FOR_WAKE registered=1 remaining_ms="
              << std::max<int64_t>(0, left.count()) << std::endl;
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_until(lock, deadline, [this] { return done_; });
  }
  source_.Stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    counts_.registration = source_.diagnostics();
    *counts = counts_;
    return registered && !failed_ &&
           (!require_complete_turn || counts_.complete_turns > 0);
  }
}

}  // namespace xiaoai_plus::native_speech
