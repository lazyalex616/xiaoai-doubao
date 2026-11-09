#include "native_speech/source.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::chrono_literals;
using xiaoai_plus::native_speech::Source;

namespace {

void Varint(uint64_t value, std::vector<uint8_t>* out) {
  while (value >= 128) {
    out->push_back(static_cast<uint8_t>((value & 127) | 128));
    value >>= 7;
  }
  out->push_back(static_cast<uint8_t>(value));
}

void Number(uint64_t key, uint64_t value, std::vector<uint8_t>* out) {
  Varint(key << 3, out);
  Varint(value, out);
}

void Bytes(uint64_t key, const std::vector<uint8_t>& value,
           std::vector<uint8_t>* out) {
  Varint((key << 3) | 2, out);
  Varint(value.size(), out);
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<uint8_t> Text(const std::string& value) {
  return {value.begin(), value.end()};
}

std::vector<uint8_t> Up(uint64_t type, uint64_t body_field = 0,
                        const std::vector<uint8_t>& body = {}) {
  std::vector<uint8_t> inner;
  Number(1, type, &inner);
  if (body_field) Bytes(body_field, body, &inner);
  std::vector<uint8_t> outer;
  Number(1, 0, &outer);
  Bytes(2, inner, &outer);
  return outer;
}

std::vector<uint8_t> Register(const std::string& codec = "pcm16") {
  std::vector<uint8_t> body;
  Bytes(1, Text("xiaomi"), &body);
  Bytes(2, Text(codec), &body);
  return Up(0, 2, body);
}

std::vector<uint8_t> RegisterNoCodecGolden() {
  // Upstream protobuf wire layout: UPWARD(0), REGISTER(0), vendor="xiaomi".
  // The optional codec field is ABSENT, unlike Register("") (present but empty).
  return {0x08, 0x00, 0x12, 0x0c, 0x08, 0x00, 0x12, 0x08,
          0x0a, 0x06, 0x78, 0x69, 0x61, 0x6f, 0x6d, 0x69};
}

std::vector<uint8_t> Prepare(uint64_t activation) {
  std::vector<uint8_t> body;
  Number(1, activation, &body);
  Number(2, 1, &body);
  return Up(1, 3, body);
}

std::vector<uint8_t> Transmitting(uint64_t stream_type,
                                  const std::vector<uint8_t>& pcm) {
  std::vector<uint8_t> body;
  Number(1, stream_type, &body);
  Bytes(2, pcm, &body);
  return Up(3, 4, body);
}

uint64_t ReadVarint(const uint8_t*& cursor, const uint8_t* end) {
  uint64_t value = 0;
  for (int shift = 0; shift <= 63; shift += 7) {
    assert(cursor < end);
    const uint8_t byte = *cursor++;
    value |= static_cast<uint64_t>(byte & 127) << shift;
    if ((byte & 128) == 0) return value;
  }
  throw std::runtime_error("bad varint");
}

uint64_t DownType(const std::vector<uint8_t>& packet) {
  const uint8_t* cursor = packet.data();
  const uint8_t* end = cursor + packet.size();
  assert(ReadVarint(cursor, end) == 8);
  assert(ReadVarint(cursor, end) == 1);
  assert(ReadVarint(cursor, end) == 26);
  const size_t inner_len = ReadVarint(cursor, end);
  assert(inner_len <= static_cast<size_t>(end - cursor));
  const uint8_t* inner_end = cursor + inner_len;
  assert(ReadVarint(cursor, inner_end) == 8);
  return ReadVarint(cursor, inner_end);
}

int Bind(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  assert(fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  assert(path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  assert(bind(fd, reinterpret_cast<sockaddr*>(&address), len) == 0);
  return fd;
}

void Send(int fd, const std::string& path, const std::vector<uint8_t>& bytes) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  assert(sendto(fd, bytes.data(), bytes.size(), 0,
                reinterpret_cast<sockaddr*>(&address), len) ==
         static_cast<ssize_t>(bytes.size()));
}

uint64_t ReceiveType(int fd) {
  pollfd descriptor{fd, POLLIN, 0};
  assert(poll(&descriptor, 1, 2000) == 1);
  std::vector<uint8_t> bytes(4096);
  const ssize_t n = recv(fd, bytes.data(), bytes.size(), 0);
  assert(n > 0);
  bytes.resize(static_cast<size_t>(n));
  return DownType(bytes);
}

struct Tmp {
  char name[64]{"/tmp/xiaoai-native-XXXXXX"};
  std::string dir;
  Tmp() {
    assert(mkdtemp(name));
    dir = name;
    assert(chmod(dir.c_str(), 0700) == 0);
  }
  ~Tmp() { rmdir(dir.c_str()); }
};

struct Observed {
  std::mutex mu;
  std::condition_variable cv;
  int ready{0};
  int wakes{0};
  int follows{0};
  int pcm{0};
  int ends{0};
  int fatals{0};
  uint64_t latest_turn{0};
  std::string fatal;

  template <typename Predicate>
  void Wait(Predicate predicate) {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, predicate));
  }
};

Source::Callbacks Callbacks(Observed* observed) {
  Source::Callbacks callbacks;
  callbacks.on_ready = [observed](const std::string&, const std::string&) {
    std::lock_guard<std::mutex> lock(observed->mu);
    ++observed->ready;
    observed->cv.notify_all();
  };
  callbacks.on_turn = [observed](uint64_t id, Source::Activation mode) {
    std::lock_guard<std::mutex> lock(observed->mu);
    if (mode == Source::Activation::kWake) ++observed->wakes;
    if (mode == Source::Activation::kFollowUp) ++observed->follows;
    observed->latest_turn = id;
    observed->cv.notify_all();
  };
  callbacks.on_pcm = [observed](uint64_t, const std::vector<uint8_t>&) {
    std::lock_guard<std::mutex> lock(observed->mu);
    ++observed->pcm;
    observed->cv.notify_all();
  };
  callbacks.on_end = [observed](uint64_t) {
    std::lock_guard<std::mutex> lock(observed->mu);
    ++observed->ends;
    observed->cv.notify_all();
  };
  callbacks.on_fatal = [observed](const std::string& reason) {
    std::lock_guard<std::mutex> lock(observed->mu);
    ++observed->fatals;
    observed->fatal = reason;
    observed->cv.notify_all();
  };
  return callbacks;
}

void HappyPath() {
  Tmp tmp;
  const std::string socket_path = tmp.dir + "/speech.usock";
  const std::string peer_path = tmp.dir + "/mipns-xiaomi";
  const int peer = Bind(peer_path);
  Source source({socket_path, tmp.dir + "/mipns-", true});
  Observed observed;
  assert(source.Start(Callbacks(&observed)));
  assert(source.socket_ready());
  assert(!source.registered());

  Send(peer, socket_path, Register());
  assert(ReceiveType(peer) == 0);
  assert(ReceiveType(peer) == 9);
  assert(source.WaitForRegistered(2s));
  observed.Wait([&] { return observed.ready == 1; });
  {
    const auto d = source.diagnostics();
    assert(d.stage == Source::RegistrationStage::kRegistered);
    assert(d.register_packets_seen == 1);
    assert(d.peer_qualified == 1);
    assert(d.codec_explicit_pcm == 1);
    assert(d.register_ready == 1);
    assert(d.fatal_class == Source::FatalClass::kNone);
    assert(d.fatal_count == 0);
  }

  Send(peer, socket_path, Prepare(0));
  assert(ReceiveType(peer) == 1);
  observed.Wait([&] { return observed.wakes == 1; });
  const uint64_t first = observed.latest_turn;
  Send(peer, socket_path, Transmitting(0, {1, 2}));  // wake audio is not ASR PCM
  Send(peer, socket_path, Transmitting(2, {1, 2, 3, 4}));
  observed.Wait([&] { return observed.pcm == 1; });
  assert(source.StopCapture(first));
  assert(ReceiveType(peer) == 2);
  assert(source.ContinueDialog(first));
  assert(ReceiveType(peer) == 3);
  assert(ReceiveType(peer) == 5);
  assert(source.ContinueDialog(first));  // Idempotent; no duplicate wire command.

  Send(peer, socket_path, Prepare(1));
  assert(ReceiveType(peer) == 1);
  observed.Wait([&] { return observed.follows == 1; });
  const uint64_t second = observed.latest_turn;
  assert(second > first);
  assert(!source.StopCapture(first));
  Send(peer, socket_path, Transmitting(2, {5, 6}));
  observed.Wait([&] { return observed.pcm == 2; });
  Send(peer, socket_path, Up(4));
  observed.Wait([&] { return observed.ends == 1; });
  Send(peer, socket_path, Transmitting(2, {7, 8}));
  assert(source.FinishDialog(second));
  assert(ReceiveType(peer) == 5);

  Send(peer, socket_path, Prepare(0));
  assert(ReceiveType(peer) == 1);
  observed.Wait([&] { return observed.wakes == 2; });
  const uint64_t timeout_turn = observed.latest_turn;
  assert(source.ContinueDialog(timeout_turn));
  assert(ReceiveType(peer) == 3);
  assert(ReceiveType(peer) == 5);
  assert(source.FinishDialog(timeout_turn));  // 30-second follow-up timeout path.
  assert(ReceiveType(peer) == 5);
  assert(!source.FinishDialog(timeout_turn));
  Send(peer, socket_path, Prepare(0));
  assert(ReceiveType(peer) == 1);
  observed.Wait([&] { return observed.wakes == 3; });
  assert(observed.latest_turn > timeout_turn);
  source.Stop();
  assert(observed.pcm == 2);
  assert(observed.fatals == 0);
  close(peer);
  unlink(peer_path.c_str());
}

void FailureCases() {
  {
    Tmp tmp;
    const std::string socket_path = tmp.dir + "/speech.usock";
    const int occupied = Bind(socket_path);
    Source source({socket_path, tmp.dir + "/", true});
    assert(!source.Start({}));
    assert(source.last_error().find("exists") != std::string::npos);
    close(occupied);
    unlink(socket_path.c_str());
  }
  for (int which = 0; which < 3; ++which) {
    Tmp tmp;
    const std::string socket_path = tmp.dir + "/speech.usock";
    const std::string peer_path = tmp.dir + "/mipns-xiaomi";
    const int peer = Bind(peer_path);
    Source source({socket_path, tmp.dir + "/mipns-", true});
    Observed observed;
    assert(source.Start(Callbacks(&observed)));
    if (which == 0) Send(peer, socket_path, Register("opus32"));
    if (which == 1) Send(peer, socket_path, {8, 0, 18, 5, 1});
    if (which == 2) unlink(socket_path.c_str());
    observed.Wait([&] { return observed.fatals == 1; });
    assert(!source.registered());
    assert(!source.last_error().empty());
    const auto d = source.diagnostics();
    if (which == 0) {
      assert(d.codec_explicit_rejected == 1);
      assert(d.fatal_class == Source::FatalClass::kCodec);
    }
    if (which == 1) assert(d.fatal_class == Source::FatalClass::kProtocol);
    if (which == 2) assert(d.fatal_class == Source::FatalClass::kSocket);
    assert(d.fatal_count == 1);
    source.Stop();
    close(peer);
    unlink(peer_path.c_str());
  }
}

void CodecFieldVariants() {
  const auto golden = RegisterNoCodecGolden();
  assert(golden.size() == 16);
  assert(Register("").size() == 18);
  assert(Register("pcm16").size() == 23);
  assert(Register("opus32").size() == 24);
  for (int which = 0; which < 4; ++which) {
    Tmp tmp;
    const std::string socket_path = tmp.dir + "/speech.usock";
    const std::string peer_path = tmp.dir + "/mipns-xiaomi";
    const int peer = Bind(peer_path);
    Source source({socket_path, tmp.dir + "/mipns-", true});
    Observed observed;
    assert(source.Start(Callbacks(&observed)));
    if (which == 0) Send(peer, socket_path, golden);
    if (which == 1) Send(peer, socket_path, Register(""));
    if (which == 2) Send(peer, socket_path, Register("pcm16"));
    if (which == 3) Send(peer, socket_path, Register("PCM"));
    assert(ReceiveType(peer) == 0);
    assert(ReceiveType(peer) == 9);
    assert(source.WaitForRegistered(2s));
    observed.Wait([&] { return observed.ready == 1; });
    const auto d = source.diagnostics();
    assert(d.stage == Source::RegistrationStage::kRegistered);
    assert(d.register_packets_seen == 1);
    assert(d.register_ready == 1);
    assert(d.fatal_class == Source::FatalClass::kNone);
    assert(d.fatal_count == 0);
    assert(d.codec_absent == static_cast<uint64_t>(which == 0));
    assert(d.codec_empty == static_cast<uint64_t>(which == 1));
    assert(d.codec_explicit_pcm == static_cast<uint64_t>(which >= 2));
    source.Stop();
    close(peer);
    unlink(peer_path.c_str());
  }
}

void PeerRejectionCounters() {
  Tmp tmp;
  const std::string socket_path = tmp.dir + "/speech.usock";
  const std::string bad_path = tmp.dir + "/other-peer";
  const std::string good_path = tmp.dir + "/mipns-xiaomi";
  const int bad = Bind(bad_path);
  const int good = Bind(good_path);
  Source source({socket_path, tmp.dir + "/mipns-", true});
  assert(source.Start({}));
  Send(bad, socket_path, RegisterNoCodecGolden());
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (source.diagnostics().peer_path_rejected == 0 &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  auto d = source.diagnostics();
  assert(d.peer_path_rejected == 1);
  assert(d.register_packets_seen == 0);
  Send(good, socket_path, RegisterNoCodecGolden());
  assert(ReceiveType(good) == 0);
  assert(ReceiveType(good) == 9);
  assert(source.WaitForRegistered(2s));
  d = source.diagnostics();
  assert(d.peer_qualified == 1);
  assert(d.codec_absent == 1);
  source.Stop();
  close(bad);
  close(good);
  unlink(bad_path.c_str());
  unlink(good_path.c_str());
}

void CallbackStop() {
  Tmp tmp;
  const std::string socket_path = tmp.dir + "/speech.usock";
  const std::string peer_path = tmp.dir + "/mipns-xiaomi";
  const int peer = Bind(peer_path);
  Source source({socket_path, tmp.dir + "/mipns-", true});
  Observed observed;
  std::mutex callback_mu;
  std::condition_variable callback_cv;
  bool entered = false;
  bool release = false;
  auto callbacks = Callbacks(&observed);
  callbacks.on_ready = [&](const std::string&, const std::string&) {
    {
      std::unique_lock<std::mutex> lock(callback_mu);
      entered = true;
      callback_cv.notify_all();
      callback_cv.wait(lock, [&] { return release; });
    }
    source.Stop();  // Must not self-join the dispatch thread.
    std::lock_guard<std::mutex> lock(observed.mu);
    ++observed.ready;
    observed.cv.notify_all();
  };
  assert(source.Start(std::move(callbacks)));
  Send(peer, socket_path, Register());
  assert(ReceiveType(peer) == 0);
  assert(ReceiveType(peer) == 9);
  {
    std::unique_lock<std::mutex> lock(callback_mu);
    assert(callback_cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  Send(peer, socket_path, Prepare(0));
  assert(ReceiveType(peer) == 1);  // A turn is queued behind the blocked callback.
  {
    std::lock_guard<std::mutex> lock(callback_mu);
    release = true;
  }
  callback_cv.notify_all();
  observed.Wait([&] { return observed.ready == 1; });
  source.Stop();  // Owner joins both threads.
  assert(observed.wakes == 0);  // Callback self-stop discards the queued turn.
  close(peer);
  unlink(peer_path.c_str());
}

void CallbackExceptionIsRedacted() {
  Tmp tmp;
  const std::string socket_path = tmp.dir + "/speech.usock";
  const std::string peer_path = tmp.dir + "/mipns-xiaomi";
  const int peer = Bind(peer_path);
  Source source({socket_path, tmp.dir + "/mipns-", true});
  Observed observed;
  auto callbacks = Callbacks(&observed);
  callbacks.on_ready = [](const std::string&, const std::string&) {
    throw std::runtime_error("secret-content");
  };
  assert(source.Start(std::move(callbacks)));
  Send(peer, socket_path, Register());
  assert(ReceiveType(peer) == 0);
  assert(ReceiveType(peer) == 9);
  observed.Wait([&] { return observed.fatals == 1; });
  assert(observed.fatal == "native speech callback exception");
  assert(source.diagnostics().fatal_class == Source::FatalClass::kCallback);
  assert(source.diagnostics().fatal_count == 1);
  source.Stop();
  close(peer);
  unlink(peer_path.c_str());
}

void CallbackQueueOverflowFailsClosed() {
  Tmp tmp;
  const std::string socket_path = tmp.dir + "/speech.usock";
  const std::string peer_path = tmp.dir + "/mipns-xiaomi";
  const int peer = Bind(peer_path);
  Source source({socket_path, tmp.dir + "/mipns-", true});
  Observed observed;
  std::mutex callback_mu;
  std::condition_variable callback_cv;
  bool entered = false;
  bool release = false;
  auto callbacks = Callbacks(&observed);
  callbacks.on_pcm = [&](uint64_t, const std::vector<uint8_t>&) {
    {
      std::unique_lock<std::mutex> lock(callback_mu);
      entered = true;
      callback_cv.notify_all();
      callback_cv.wait(lock, [&] { return release; });
    }
    std::lock_guard<std::mutex> lock(observed.mu);
    ++observed.pcm;
    observed.cv.notify_all();
  };
  assert(source.Start(std::move(callbacks)));
  Send(peer, socket_path, Register());
  assert(ReceiveType(peer) == 0);
  assert(ReceiveType(peer) == 9);
  Send(peer, socket_path, Prepare(0));
  assert(ReceiveType(peer) == 1);
  Send(peer, socket_path, Transmitting(2, {1, 2}));
  {
    std::unique_lock<std::mutex> lock(callback_mu);
    assert(callback_cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const auto len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                           socket_path.size() + 1);
  const auto frame = Transmitting(2, {3, 4});
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  size_t sent = 0;
  while (sent < 513 && std::chrono::steady_clock::now() < deadline) {
    const ssize_t n = sendto(peer, frame.data(), frame.size(), MSG_DONTWAIT,
                             reinterpret_cast<sockaddr*>(&address), len);
    if (n == static_cast<ssize_t>(frame.size())) {
      ++sent;
    } else {
      assert(n < 0 &&
             (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS));
      std::this_thread::sleep_for(1ms);
    }
  }
  assert(sent == 513);
  while (source.socket_ready() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  assert(!source.socket_ready());
  {
    std::lock_guard<std::mutex> lock(callback_mu);
    release = true;
  }
  callback_cv.notify_all();
  observed.Wait([&] { return observed.fatals == 1; });
  assert(observed.fatal == "native speech callback queue overflow");
  assert(source.diagnostics().fatal_class == Source::FatalClass::kQueue);
  assert(source.diagnostics().fatal_count == 1);
  source.Stop();
  assert(observed.pcm == 1);
  close(peer);
  unlink(peer_path.c_str());
}

}  // namespace

int main() {
  HappyPath();
  FailureCases();
  CodecFieldVariants();
  PeerRejectionCounters();
  CallbackStop();
  CallbackExceptionIsRedacted();
  CallbackQueueOverflowFailsClosed();
}
