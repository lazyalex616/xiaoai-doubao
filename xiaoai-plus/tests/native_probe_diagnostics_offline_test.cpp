#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "native_speech/probe.hpp"

using xiaoai_plus::native_speech::Probe;
using xiaoai_plus::native_speech::Source;

namespace {

void Varint(uint64_t value, std::vector<uint8_t>* out) {
  while (value >= 128) {
    out->push_back(static_cast<uint8_t>((value & 127) | 128));
    value >>= 7;
  }
  out->push_back(static_cast<uint8_t>(value));
}

void Number(uint64_t field, uint64_t value, std::vector<uint8_t>* out) {
  Varint(field << 3, out);
  Varint(value, out);
}

void Bytes(uint64_t field, const std::vector<uint8_t>& value,
           std::vector<uint8_t>* out) {
  Varint((field << 3) | 2, out);
  Varint(value.size(), out);
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<uint8_t> Register(const char* codec) {
  std::vector<uint8_t> body;
  const std::vector<uint8_t> vendor{'x', 'i', 'a', 'o', 'm', 'i'};
  Bytes(1, vendor, &body);
  if (codec) {
    const std::vector<uint8_t> c(codec, codec + std::strlen(codec));
    Bytes(2, c, &body);
  }
  std::vector<uint8_t> inner;
  Number(1, 0, &inner);
  Bytes(2, body, &inner);
  std::vector<uint8_t> outer;
  Number(1, 0, &outer);
  Bytes(2, inner, &outer);
  return outer;
}

struct RunResult {
  bool okay{false};
  Probe::Counts counts;
};

RunResult Run(const char* codec, bool send_packet) {
  char pattern[] = "/tmp/xiaoai-native-probe-diag-XXXXXX";
  char* directory = mkdtemp(pattern);
  assert(directory);
  const std::string root(directory);
  const std::string socket_path = root + "/speech.usock";
  const std::string peer_path = root + "/mipns-xiaomi";
  Probe probe(Source::Options{socket_path, root + "/mipns-", true});
  RunResult result;
  std::thread worker([&] {
    result.okay = probe.Run(std::chrono::seconds(1), &result.counts, false);
  });
  for (int i = 0; i < 100 && access(socket_path.c_str(), F_OK) != 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(access(socket_path.c_str(), F_OK) == 0);
  int peer_fd = -1;
  if (send_packet) {
    peer_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    assert(peer_fd >= 0);
    sockaddr_un peer{};
    peer.sun_family = AF_UNIX;
    assert(peer_path.size() < sizeof(peer.sun_path));
    std::memcpy(peer.sun_path, peer_path.c_str(), peer_path.size() + 1);
    assert(bind(peer_fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
    const auto packet = Register(codec);
    if (!codec) assert(packet.size() == 16);
    assert(sendto(peer_fd, packet.data(), packet.size(), 0,
                  reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
           static_cast<ssize_t>(packet.size()));
  }
  worker.join();
  if (peer_fd >= 0) close(peer_fd);
  unlink(peer_path.c_str());
  unlink(socket_path.c_str());
  assert(rmdir(root.c_str()) == 0);
  return result;
}

}  // namespace

int main() {
  const auto none = Run(nullptr, false);
  assert(!none.okay);
  assert(none.counts.registration.stage == Source::RegistrationStage::kSocketBound);
  assert(none.counts.registration.datagrams_seen == 0);
  assert(none.counts.registration.fatal_class == Source::FatalClass::kNone);

  const auto unsupported = Run("opus32", true);
  assert(!unsupported.okay);
  assert(unsupported.counts.registration.register_packets_seen == 1);
  assert(unsupported.counts.registration.codec_explicit_rejected == 1);
  assert(unsupported.counts.registration.fatal_class == Source::FatalClass::kCodec);

  const auto default_pcm = Run(nullptr, true);
  assert(default_pcm.okay);
  assert(default_pcm.counts.registered);
  assert(default_pcm.counts.registration.codec_absent == 1);
  assert(default_pcm.counts.registration.register_ready == 1);
  assert(default_pcm.counts.registration.stage == Source::RegistrationStage::kRegistered);

  std::cout << "native probe diagnostic categories PASS" << std::endl;
}
