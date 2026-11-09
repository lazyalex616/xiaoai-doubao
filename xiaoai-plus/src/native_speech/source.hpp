#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>

namespace xiaoai_plus::native_speech {

// This module owns only the Unix datagram endpoint and protocol. A separate,
// bounded launcher owns native-service takeover and restoration.
class Source {
 public:
  static constexpr const char* kSystemSocketPath =
      "/tmp/mico_aivs_lab/usock/speech.usock";

  enum class Activation { kWake, kFollowUp, kUnknown };
  enum class RegistrationStage {
    kNotStarted, kSocketBound, kPeerQualified, kRegisterParsed,
    kCodecAccepted, kRegistered
  };
  enum class FatalClass {
    kNone, kSocket, kProtocol, kCodec, kSend, kQueue, kCallback
  };

  struct Diagnostics {
    RegistrationStage stage{RegistrationStage::kNotStarted};
    FatalClass fatal_class{FatalClass::kNone};
    uint64_t fatal_count{0};
    uint64_t datagrams_seen{0};
    uint64_t peer_path_rejected{0};
    uint64_t peer_owner_rejected{0};
    uint64_t peer_qualified{0};
    uint64_t register_packets_seen{0};
    uint64_t codec_absent{0};
    uint64_t codec_empty{0};
    uint64_t codec_explicit_pcm{0};
    uint64_t codec_explicit_rejected{0};
    uint64_t register_ready{0};
  };

  struct Options {
    std::string socket_path{kSystemSocketPath};
    std::string expected_peer_prefix{"/tmp/mipns/"};
    bool allow_test_path{false};
  };

  struct Callbacks {
    std::function<void(const std::string& vendor, const std::string& codec)> on_ready;
    std::function<void(uint64_t turn_id, Activation activation)> on_turn;
    std::function<void(uint64_t turn_id, const std::vector<uint8_t>& pcm_s16le)> on_pcm;
    std::function<void(uint64_t turn_id)> on_end;
    std::function<void(uint64_t turn_id)> on_cancel;
    std::function<void(const std::string& reason)> on_fatal;
  };

  Source();
  explicit Source(Options options);
  ~Source();
  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;

  // Start binds only an absent socket path inside an existing private directory.
  // It never stops native services, removes an existing socket, or reads config.
  bool Start(Callbacks callbacks);
  void Stop();
  bool socket_ready() const;
  bool registered() const;
  bool WaitForRegistered(std::chrono::milliseconds timeout);
  std::string last_error() const;
  Diagnostics diagnostics() const;
  static const char* RegistrationStageName(RegistrationStage stage);
  static const char* FatalClassName(FatalClass fatal_class);

  // Commands are turn-scoped. ContinueDialog sends EXPECT_SPEECH followed by
  // DIALOG_FINISH; the caller separately requests pnshelper follow-up capture.
  bool StopCapture(uint64_t turn_id);
  bool ContinueDialog(uint64_t turn_id);
  bool FinishDialog(uint64_t turn_id);

 private:
  struct Event {
    enum class Kind { kReady, kTurn, kPcm, kEnd, kCancel, kFatal } kind;
    uint64_t turn_id{0};
    Activation activation{Activation::kUnknown};
    std::string text;
    std::string codec;
    std::vector<uint8_t> pcm;
  };

  void ReceiveLoop();
  void DispatchLoop();
  bool HandlePacket(const uint8_t* data, size_t size,
                    const sockaddr_un& sender, socklen_t sender_len,
                    FatalClass* failure_class);
  bool SendDownLocked(uint64_t kind, const std::string& dialog_id,
                      uint64_t body_field = 0,
                      const std::vector<uint8_t>& body = {});
  bool CheckTurnLocked(uint64_t turn_id);
  void Queue(Event event);
  void Fail(const std::string& reason, FatalClass fatal_class);

  Options options_;
  Callbacks callbacks_;
  mutable std::mutex state_mu_;
  std::condition_variable registration_cv_;
  int fd_{-1};
  uint64_t socket_dev_{0};
  uint64_t socket_ino_{0};
  bool registered_{false};
  sockaddr_un peer_{};
  socklen_t peer_len_{0};
  uint64_t next_turn_id_{0};
  uint64_t current_turn_id_{0};
  bool stream_active_{false};
  bool stop_capture_sent_{false};
  bool turn_continued_{false};
  bool turn_finished_{false};
  uint64_t chat_count_{0};
  uint64_t dialog_count_{0};
  std::string dialog_id_;
  std::string last_error_;
  Diagnostics diagnostics_;
  std::atomic<FatalClass> fatal_class_{FatalClass::kNone};
  std::atomic<uint64_t> fatal_count_{0};

  std::mutex queue_mu_;
  std::condition_variable queue_cv_;
  std::deque<Event> queue_;
  size_t queued_pcm_bytes_{0};
  bool dispatch_stop_{false};
  std::atomic<bool> running_{false};
  std::thread receive_thread_;
  std::thread dispatch_thread_;
};

}  // namespace xiaoai_plus::native_speech
