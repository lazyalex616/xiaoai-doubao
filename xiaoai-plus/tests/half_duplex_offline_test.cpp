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

int main() {
  char root[] = "/tmp/xiaoai-half-duplex-XXXXXX";
  assert(mkdtemp(root));
  const int port = ix::getFreePort();
  assert(port > 0);
  ix::WebSocketServer server(port, "127.0.0.1");
  std::mutex mu;
  std::condition_variable cv;
  std::vector<std::string> events;
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type != ix::WebSocketMessageType::Message) return;
    const auto type = nlohmann::json::parse(msg->str).at("type").get<std::string>();
    {
      std::lock_guard<std::mutex> lock(mu);
      events.push_back(type);
      cv.notify_all();
    }
    if (type == "session.create") {
      ws.sendText(R"({"type":"session.created","session":{"id":"offline"}})");
      ws.sendText(R"({"type":"response.output_audio.delta","delta":"AQI="})");
      ws.sendText(R"({"type":"conversation.item.input_audio_transcription.started"})");
    } else if (type == "response.cancel") {
      ws.sendText(R"({"type":"response.output_audio.done","status_code":20000002})");
    } else if (type == "session.close") {
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());
  xiaoai_plus::config::Config cfg;
  cfg.realtime.api_key = "fake-offline-key";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  cfg.conversation.usage_dir = std::string(root) + "/usage";
  std::atomic<int> exit_intents{0};
  std::atomic<int> user_activity{0};
  xiaoai_plus::realtime::Client::Callbacks callbacks;
  callbacks.on_exit_intent = [&] { ++exit_intents; cv.notify_all(); };
  callbacks.on_user_activity = [&] { ++user_activity; };
  xiaoai_plus::realtime::Client client(cfg, callbacks);
  assert(client.Start() && client.StartSession(3s));
  client.SetPlaybackActive(true);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] {
      return std::find(events.begin(), events.end(), "input_audio_mute.commit") != events.end();
    }));
    assert(std::find(events.begin(), events.end(), "response.cancel") == events.end());
  }
  assert(user_activity == 0);
  const auto count_appends = [&] {
    std::lock_guard<std::mutex> lock(mu);
    return std::count(events.begin(), events.end(), "input_audio_buffer.append");
  };
  const auto before = count_appends();
  assert(client.EnqueueAudio(std::vector<uint8_t>(640, 0)));
  std::this_thread::sleep_for(80ms);
  assert(count_appends() == before);
  client.SetPlaybackActive(false);
  assert(client.EnqueueAudio(std::vector<uint8_t>(640, 0)));
  std::this_thread::sleep_for(150ms);
  assert(count_appends() == before);
  std::this_thread::sleep_for(190ms);
  assert(client.EnqueueAudio(std::vector<uint8_t>(640, 0)));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] {
      return std::count(events.begin(), events.end(), "input_audio_buffer.append") > before;
    }));
    assert(std::find(events.begin(), events.end(), "input_audio_unmute.commit") != events.end());
  }
  // Simulate missing output_audio.done. Recovery must unmute without waiting
  // for a new microphone chunk.
  const auto count_type = [&](const std::string& type) {
    std::lock_guard<std::mutex> lock(mu);
    return std::count(events.begin(), events.end(), type);
  };
  const auto mute_before = count_type("input_audio_mute.commit");
  client.SetPlaybackActive(true);
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] {
      return std::count(events.begin(), events.end(), "input_audio_mute.commit") > mute_before;
    }));
  }
  const auto unmute_before = count_type("input_audio_unmute.commit");
  client.RecoverStalledPlayback();
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] {
      return std::count(events.begin(), events.end(), "input_audio_unmute.commit") > unmute_before;
    }));
  }
  assert(client.InterruptResponse());
  for (int i = 0; i < 100 && exit_intents == 0; ++i) std::this_thread::sleep_for(10ms);
  assert(exit_intents == 1);
  assert(client.FinishSession(2s));
  client.Stop();
  server.stop();
  std::filesystem::remove_all(root);
}
