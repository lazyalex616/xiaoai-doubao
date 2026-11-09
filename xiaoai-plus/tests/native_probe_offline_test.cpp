#include "native_speech/probe.hpp"

#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace std::chrono_literals;
using xiaoai_plus::native_speech::Probe;

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
std::vector<uint8_t> Register() {
  std::vector<uint8_t> body;
  Bytes(1, {'x', 'i', 'a', 'o', 'm', 'i'}, &body);
  Bytes(2, {'p', 'c', 'm', '1', '6'}, &body);
  return Up(0, 2, body);
}
std::vector<uint8_t> Prepare() {
  std::vector<uint8_t> body;
  Number(1, 0, &body);
  Number(2, 1, &body);
  return Up(1, 3, body);
}
std::vector<uint8_t> Pcm() {
  std::vector<uint8_t> body;
  Number(1, 2, &body);
  Bytes(2, {1, 2, 3, 4}, &body);
  return Up(3, 4, body);
}
std::string TempDir() {
  char name[] = "/tmp/xiaoai-probe-XXXXXX";
  assert(mkdtemp(name));
  assert(chmod(name, 0700) == 0);
  return name;
}
int Bind(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  assert(fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  assert(path.size() < sizeof(address.sun_path));
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  assert(bind(fd, reinterpret_cast<sockaddr*>(&address),
              static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)) == 0);
  return fd;
}
void Send(int fd, const std::string& path, const std::vector<uint8_t>& packet) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  assert(sendto(fd, packet.data(), packet.size(), 0,
                reinterpret_cast<sockaddr*>(&address),
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)) ==
         static_cast<ssize_t>(packet.size()));
}
void WaitForSocket(const std::string& path) {
  struct stat info{};
  for (int i = 0; i < 100 && stat(path.c_str(), &info) != 0; ++i)
    std::this_thread::sleep_for(5ms);
  assert(stat(path.c_str(), &info) == 0);
}
void RunCase(bool send_wake, bool send_pcm, bool send_end,
             bool expect_complete, bool expect_success) {
  const std::string dir = TempDir();
  const std::string socket_path = dir + "/speech.usock";
  const std::string peer_path = dir + "/mipns-xiaomi";
  const int peer = Bind(peer_path);
  Probe probe({socket_path, dir + "/mipns-", true});
  Probe::Counts counts;
  bool result = false;
  std::thread worker([&] { result = probe.Run(1s, &counts, expect_complete); });
  WaitForSocket(socket_path);
  Send(peer, socket_path, Register());
  std::this_thread::sleep_for(30ms);
  if (send_wake) Send(peer, socket_path, Prepare());
  if (send_pcm) Send(peer, socket_path, Pcm());
  if (send_end) Send(peer, socket_path, Up(4));
  worker.join();
  assert(result == expect_success);
  assert(counts.registered);
  assert(counts.wakes == static_cast<uint64_t>(send_wake));
  assert(counts.pcm_packets == static_cast<uint64_t>(send_pcm));
  assert(counts.ended == static_cast<uint64_t>(send_end));
  assert(counts.complete_turns == static_cast<uint64_t>(send_wake && send_pcm && send_end));
  close(peer);
  unlink(peer_path.c_str());
  rmdir(dir.c_str());
}
}  // namespace

int main() {
  RunCase(false, false, false, false, true);   // Registration alone is explicit.
  RunCase(false, false, false, true, false);  // Never a complete voice PASS.
  RunCase(true, true, false, true, false);    // Wake and PCM without End fail.
  RunCase(true, true, true, true, true);      // One matched turn passes.
}
