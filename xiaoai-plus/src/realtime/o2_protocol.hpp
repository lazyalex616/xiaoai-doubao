#pragma once
#include <string>
#include <vector>
#include "config/config.hpp"
#include "realtime/frame_codec.hpp"

namespace xiaoai_plus::realtime::o2 {
inline constexpr const char* kUrl = "wss://openspeech.bytedance.com/api/v3/realtime/dialogue";
// Wire adapter only. Identity, memory, playback and billing use the same v3 events.
class Session {
 public:
  void Reset();
  bool connected() const { return connected_; }
  std::string Encode(const std::string& event, const config::Realtime& cfg);
  std::string Audio(const std::vector<uint8_t>& pcm) const;
  std::vector<std::string> Receive(const std::string& binary);
 private:
  std::string Json(protocol::EventId, const nlohmann::json&) const;
  std::string session_id_, question_, reply_, asr_, text_, canceled_question_;
  bool connected_{false}, canceled_{false}, output_allowed_{true};
  uint64_t serial_{0};
};
}  // namespace xiaoai_plus::realtime::o2
