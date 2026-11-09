#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "app/native_app.hpp"

using namespace std::chrono_literals;

namespace {
void Varint(uint64_t value, std::vector<uint8_t>* out) {
  while (value >= 128) { out->push_back(static_cast<uint8_t>((value & 127) | 128)); value >>= 7; }
  out->push_back(static_cast<uint8_t>(value));
}
void Number(uint64_t key, uint64_t value, std::vector<uint8_t>* out) {
  Varint(key << 3, out); Varint(value, out);
}
void Bytes(uint64_t key, const std::vector<uint8_t>& value, std::vector<uint8_t>* out) {
  Varint((key << 3) | 2, out); Varint(value.size(), out);
  out->insert(out->end(), value.begin(), value.end());
}
std::vector<uint8_t> Up(uint64_t type, uint64_t body_field = 0,
                        const std::vector<uint8_t>& body = {}) {
  std::vector<uint8_t> inner, outer;
  Number(1, type, &inner);
  if (body_field) Bytes(body_field, body, &inner);
  Number(1, 0, &outer); Bytes(2, inner, &outer);
  return outer;
}
std::vector<uint8_t> Register() {
  std::vector<uint8_t> body;
  Bytes(1, {'x', 'i', 'a', 'o', 'm', 'i'}, &body);
  Bytes(2, {'p', 'c', 'm', '1', '6'}, &body);
  return Up(0, 2, body);
}
std::vector<uint8_t> Prepare(uint64_t mode) {
  std::vector<uint8_t> body;
  Number(1, mode, &body); Number(2, 1, &body);
  return Up(1, 3, body);
}
std::vector<uint8_t> Audio() {
  std::vector<uint8_t> body;
  Number(1, 2, &body); Bytes(2, std::vector<uint8_t>(640, 0), &body);
  return Up(3, 4, body);
}
int Bind(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  assert(fd >= 0);
  sockaddr_un addr{}; addr.sun_family = AF_UNIX;
  assert(path.size() < sizeof(addr.sun_path));
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  assert(bind(fd, reinterpret_cast<sockaddr*>(&addr),
              static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)) == 0);
  return fd;
}
void Send(int fd, const std::string& path, const std::vector<uint8_t>& bytes) {
  sockaddr_un addr{}; addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
  assert(sendto(fd, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr),
                static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1)) ==
         static_cast<ssize_t>(bytes.size()));
}
uint64_t ReadVarint(const uint8_t*& ptr, const uint8_t* end) {
  uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    assert(ptr < end);
    const uint8_t b = *ptr++;
    value |= static_cast<uint64_t>(b & 127) << shift;
    if (!(b & 128)) return value;
  }
  assert(false); return 0;
}
uint64_t ReceiveType(int fd) {
  pollfd pfd{fd, POLLIN, 0};
  assert(poll(&pfd, 1, 5000) == 1);
  uint8_t bytes[1024];
  const ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
  assert(n > 0);
  const uint8_t* ptr = bytes; const uint8_t* end = bytes + n;
  assert(ReadVarint(ptr, end) == 8); assert(ReadVarint(ptr, end) == 1);
  assert(ReadVarint(ptr, end) == 26);
  const size_t size = ReadVarint(ptr, end); assert(size <= static_cast<size_t>(end - ptr));
  assert(ReadVarint(ptr, end) == 8);
  return ReadVarint(ptr, end);
}
struct Temp {
  char name[64]{"/tmp/xiaoai-native-app-XXXXXX"};
  std::string path;
  Temp() { assert(mkdtemp(name)); path = name; assert(chmod(path.c_str(), 0700) == 0); }
  ~Temp() { std::filesystem::remove_all(path + "/usage"); rmdir(path.c_str()); }
};
void WaitForSocket(const std::string& path) {
  for (int i = 0; i < 300 && access(path.c_str(), F_OK) != 0; ++i)
    std::this_thread::sleep_for(10ms);
  assert(access(path.c_str(), F_OK) == 0);
}

void RegistrationTimeout() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, false, opts, [] { return true; }, 1s);
  bool result = true;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  thread.join();
  assert(!result);
  assert(access(speech.c_str(), F_OK) != 0);
}

void CloudHandshakeTimeout() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) ++connections;
    if (msg->type == ix::WebSocketMessageType::Message &&
        nlohmann::json::parse(msg->str).at("type") == "session.create") ++creates;
    // Deliberately withhold session.created; the first phrase must fail boundedly.
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, true, opts, [] { return true; }, 2s, 1s);
  bool result = true;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  thread.join();
  assert(!result && connections == 1 && creates == 1);
  assert(access(speech.c_str(), F_OK) != 0);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}

void FollowUpTimeoutReturnsToWake() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0}, commits{0}, closes{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) { ++connections; return; }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    const std::string type = nlohmann::json::parse(msg->str).at("type").get<std::string>();
    if (type == "session.create") {
      ++creates;
      ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
    } else if (type == "input_audio_buffer.commit") {
      ++commits;
      ws.sendText(R"({"type":"response.output_audio.done"})");
      ws.sendText(R"({"type":"response.done","usage":{"total_tokens":1}})");
    } else if (type == "session.close") {
      ++closes;
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  cfg.conversation.idle_seconds = 1;
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, false, opts, [] { return true; }, 2s, 2s);
  bool result = false;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  assert(ReceiveType(peer_fd) == 3 && ReceiveType(peer_fd) == 5);
  // The same native turn must accept Finish after Continue at idle expiry.
  assert(ReceiveType(peer_fd) == 5);
  for (int i = 0; i < 200 && closes == 0; ++i) std::this_thread::sleep_for(10ms);
  assert(closes == 1);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  for (int i = 0; i < 200 && creates < 2; ++i) std::this_thread::sleep_for(10ms);
  assert(connections == 2 && creates == 2 && commits == 1);
  app.RequestStop();
  thread.join();
  assert(result);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}

void IncompleteAnswerCloses() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0}, commits{0}, closes{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) { ++connections; return; }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    const std::string type = nlohmann::json::parse(msg->str).at("type").get<std::string>();
    if (type == "session.create") {
      ++creates;
      ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
    } else if (type == "input_audio_buffer.commit") {
      ++commits;
      // Simulate a stalled reply: neither output_audio.done nor response.done.
    } else if (type == "session.close") {
      ++closes;
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, true, opts, [] { return true; }, 2s, 2s, 1s);
  bool result = false;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  assert(ReceiveType(peer_fd) == 5);  // Answer timeout finishes the native turn.
  for (int i = 0; i < 200 && closes == 0; ++i) std::this_thread::sleep_for(10ms);
  assert(connections == 1 && creates == 1 && commits == 1 && closes == 1);
  app.RequestStop();
  thread.join();
  assert(result);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}

void CloudDisconnectCloses() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) { ++connections; return; }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    if (nlohmann::json::parse(msg->str).at("type") == "session.create") {
      ++creates;
      ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
      ws.close();
    }
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, true, opts, [] { return true; }, 2s, 2s);
  bool result = true;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  thread.join();
  assert(!result && connections == 1 && creates == 1);
  assert(access(speech.c_str(), F_OK) != 0);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}

void PlaybackFailureCloses() {
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0}, closes{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) { ++connections; return; }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    const std::string type = nlohmann::json::parse(msg->str).at("type").get<std::string>();
    if (type == "session.create") {
      ++creates;
      ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
    } else if (type == "input_audio_buffer.commit") {
      ws.sendText(R"({"type":"response.output_audio.delta","delta":"AQI="})");
      ws.sendText(R"({"type":"response.output_audio.done"})");
      ws.sendText(R"({"type":"response.done","usage":{"total_tokens":1}})");
    } else if (type == "session.close") {
      ++closes;
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, true, opts, [] { return true; }, 2s, 2s);
  bool result = true;
  std::thread thread([&] { result = app.Run(); });
  WaitForSocket(speech);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  thread.join();
  assert(!result && connections == 1 && creates == 1 && closes == 1);
  assert(access(speech.c_str(), F_OK) != 0);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}
}  // namespace

int main() {
  RegistrationTimeout();
  CloudHandshakeTimeout();
  FollowUpTimeoutReturnsToWake();
  IncompleteAnswerCloses();
  CloudDisconnectCloses();
  PlaybackFailureCloses();
  Temp temp;
  const std::string speech = temp.path + "/speech.usock";
  const std::string peer = temp.path + "/mipns-xiaomi";
  const int peer_fd = Bind(peer);

  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::atomic<int> connections{0}, creates{0}, appends{0}, commits{0}, closes{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) { ++connections; return; }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    assert(!msg->binary);
    const std::string type = nlohmann::json::parse(msg->str).at("type").get<std::string>();
    if (type == "session.create") {
      ++creates;
      ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
    } else if (type == "input_audio_buffer.append") {
      ++appends;
    } else if (type == "input_audio_buffer.commit") {
      const int turn = ++commits;
      if (turn == 2) {
        ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"退出"})");
      } else {
        ws.sendText(R"({"type":"response.output_audio.done"})");
        ws.sendText(R"({"type":"response.done","usage":{"total_tokens":1}})");
      }
    } else if (type == "session.close") {
      ++closes;
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());

  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.conversation.usage_dir = temp.path + "/usage";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  cfg.conversation.idle_seconds = 1;  // host-only fast timeout if needed
  std::atomic<int> followups{0};
  xiaoai_plus::native_speech::Source::Options opts{speech, temp.path + "/mipns-", true};
  xiaoai_plus::app::NativeApp app(cfg, false, opts, [&] { ++followups; return true; });
  bool run_result = false;
  std::thread thread([&] { run_result = app.Run(); });
  for (int i = 0; i < 300 && access(speech.c_str(), F_OK) != 0; ++i)
    std::this_thread::sleep_for(10ms);
  assert(access(speech.c_str(), F_OK) == 0);
  Send(peer_fd, speech, Register());
  assert(ReceiveType(peer_fd) == 0 && ReceiveType(peer_fd) == 9);
  Send(peer_fd, speech, Prepare(0));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  assert(ReceiveType(peer_fd) == 3);  // EXPECT_SPEECH after first cloud turn
  assert(ReceiveType(peer_fd) == 5);  // DIALOG_FINISH continue
  for (int i = 0; i < 300 && followups == 0; ++i) std::this_thread::sleep_for(10ms);
  assert(followups == 1);
  assert(connections == 1 && creates == 1 && commits == 1 && appends == 1);

  // Duplicate END and late PCM from the old turn must not reach cloud.
  Send(peer_fd, speech, Up(4));
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Prepare(1));
  assert(ReceiveType(peer_fd) == 1);
  Send(peer_fd, speech, Audio());
  Send(peer_fd, speech, Up(4));
  assert(ReceiveType(peer_fd) == 5);  // explicit exit finishes native dialog
  for (int i = 0; i < 300 && closes == 0; ++i) std::this_thread::sleep_for(10ms);
  assert(closes == 1 && commits == 2 && appends == 2 && creates == 1 && connections == 1);
  app.RequestStop();
  thread.join();
  assert(run_result);
  server.stop();
  close(peer_fd); unlink(peer.c_str());
}
