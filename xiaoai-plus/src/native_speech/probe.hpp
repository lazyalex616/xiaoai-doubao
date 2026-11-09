#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include "native_speech/source.hpp"

namespace xiaoai_plus::native_speech {

// Single-turn native frontend check. This type has no config, Client,
// WebSocket, or audio output dependency; it never opens a paid connection.
class Probe {
 public:
  struct Counts {
    bool registered{false};
    uint64_t wakes{0};
    uint64_t followups{0};
    uint64_t pcm_packets{0};
    uint64_t pcm_bytes{0};
    uint64_t ended{0};
    uint64_t complete_turns{0};
    Source::Diagnostics registration{};
  };

  explicit Probe(Source::Options options = {});
  bool Run(std::chrono::seconds maximum, Counts* counts,
           bool require_complete_turn = false);

 private:
  Source source_;
  std::mutex mu_;
  std::condition_variable cv_;
  Counts counts_;
  bool done_{false};
  bool failed_{false};
  uint64_t active_turn_{0};
  bool active_turn_pcm_{false};
  bool active_turn_wake_{false};
};

}  // namespace xiaoai_plus::native_speech
