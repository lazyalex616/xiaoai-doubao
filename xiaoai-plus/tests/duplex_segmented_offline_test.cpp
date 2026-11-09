#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

#include "realtime/client.hpp"

using namespace std::chrono_literals;
using xiaoai_plus::realtime::Client;

int main() {
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> events;
  std::atomic<int> connections{0};
  std::atomic<bool> error_on_append{false};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      ++connections;
      return;
    }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    assert(!msg->binary);
    const auto value = nlohmann::json::parse(msg->str);
    const std::string type = value.at("type").get<std::string>();
    {
      std::lock_guard<std::mutex> lock(mu);
      events.push_back(type);
      cv.notify_all();
    }
    if (type == "session.create") {
      ws.sendText(R"({"type":"session.created","session":{"id":"offline"}})");
    } else if (type == "input_audio_buffer.append" && error_on_append.load()) {
      error_on_append = false;
      ws.sendText(R"({"type":"error","error":{"code":400}})");
    } else if (type == "input_audio_buffer.commit") {
      ws.sendText(R"({"type":"response.done","usage":{"total_tokens":1}})");
    } else if (type == "session.close") {
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());

  xiaoai_plus::config::Config cfg;
  char usage_root[] = "/tmp/xiaoai-segment-usage-XXXXXX";
  assert(mkdtemp(usage_root));
  cfg.conversation.usage_dir = std::string(usage_root) + "/usage";
  cfg.realtime.api_key = "fake-offline-key";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  std::atomic<int> completed{0};
  std::atomic<int> closed{0};
  Client::Callbacks cb;
  cb.on_chat_ended = [&] { ++completed; cv.notify_all(); };
  cb.on_session_closed = [&](const std::string&) { ++closed; cv.notify_all(); };
  Client client(cfg, cb, true, true);
  assert(client.Start());
  assert(client.StartSession(3s));
  assert(client.EnqueueAudio(std::vector<uint8_t>(960, 0)));
  assert(client.EnqueueAudio(std::vector<uint8_t>(960, 0)));
  assert(client.EnqueueAudio(std::vector<uint8_t>(960, 0)));
  std::this_thread::sleep_for(300ms);
  {
    std::lock_guard<std::mutex> lock(mu);
    assert(std::count(events.begin(), events.end(), "input_audio_buffer.commit") == 0);
    assert(std::count(events.begin(), events.end(), "input_audio_mute.commit") == 0);
  }
  assert(client.EndInputTurn());
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 3s, [&] {
      return std::count(events.begin(), events.end(), "input_audio_mute.commit") == 1 &&
             completed == 1;
    }));
  }
  assert(completed == 1);
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  assert(client.EndInputTurn());
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 3s, [&] {
      return std::count(events.begin(), events.end(), "input_audio_mute.commit") == 2 &&
             completed == 2;
    }));
  }
  assert(completed == 2);
  {
    std::lock_guard<std::mutex> lock(mu);
    const std::vector<std::string> expected = {
      "session.create",
      "input_audio_buffer.append", "input_audio_buffer.append",
      "input_audio_buffer.append", "input_audio_buffer.append",
      "input_audio_buffer.append", "input_audio_buffer.commit", "input_audio_mute.commit",
      "input_audio_unmute.commit", "input_audio_buffer.append", "input_audio_buffer.append",
      "input_audio_buffer.append", "input_audio_buffer.append",
      "input_audio_buffer.append", "input_audio_buffer.commit", "input_audio_mute.commit"
    };
    assert(events == expected);
  }
  assert(client.FinishSession(2s));
  assert(closed == 1 && connections == 1);
  assert(!client.StartSession(2s));  // one paid connection budget per process
  client.Stop();

  // A server error before a native turn ends must prevent commit and redial.
  error_on_append = true;
  Client failed(cfg, cb, true, true);
  assert(failed.Start());
  assert(failed.StartSession(3s));
  const int before = connections;
  assert(failed.EnqueueAudio(std::vector<uint8_t>(640, 0)));
  for (int i = 0; i < 100 && closed < 2; ++i) std::this_thread::sleep_for(10ms);
  assert(closed >= 2);
  assert(!failed.EndInputTurn());
  assert(!failed.StartSession(2s));
  assert(connections == before);
  failed.Stop();
  server.stop();
  std::filesystem::remove_all(usage_root);
}
