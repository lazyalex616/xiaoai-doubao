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
  std::atomic<int> connections{0};
  std::atomic<int> appends{0};
  std::atomic<int> session_creates{0};
  std::atomic<int> cancels{0};
  std::atomic<int> closes{0};
  std::atomic<bool> ignore_create{false};
  std::atomic<bool> error_on_create{false};
  std::atomic<bool> drop_on_create{false};
  std::atomic<bool> idle_release_on_hello{false};
  std::atomic<bool> server_close_on_hello{false};
  std::atomic<bool> saw_binary{false};
  std::atomic<bool> saw_key{false};
  std::mutex append_mu;
  std::vector<std::chrono::steady_clock::time_point> append_times;
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,
                                         ix::WebSocket& ws,
                                         const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) {
      ++connections;
      auto it = msg->openInfo.headers.find("X-Api-Key");
      if (it != msg->openInfo.headers.end() && it->second == "fake-offline-key") saw_key = true;
      return;
    }
    if (msg->type != ix::WebSocketMessageType::Message) return;
    if (msg->binary) { saw_binary = true; return; }
    const auto json = nlohmann::json::parse(msg->str);
    const auto type = json.at("type").get<std::string>();
    if (type == "session.create") {
      ++session_creates;
      assert(json.at("session").at("instructions").get<std::string>().find("2 short sentences, never more than 3") != std::string::npos);
      assert(json.at("session").at("model") == "1.2.6.1");
      assert(json.at("session").at("audio").at("output").at("format").at("type") == "pcm");
      assert(!json.at("session").contains("extension"));
      if (drop_on_create.load()) ws.close();
      else if (error_on_create.load()) ws.sendText(R"({"type":"error","error":{"code":401}})");
      else if (!ignore_create.load()) ws.sendText(R"({"type":"session.created","session":{"id":"mock"}})");
    } else if (type == "speech_text_buffer.commit" && idle_release_on_hello.load()) {
      ws.sendText(R"({"type":"error","error":{"code":45000003}})");
    } else if (type == "speech_text_buffer.commit" && server_close_on_hello.load()) {
      ws.sendText(R"({"type":"session.closed"})");
    } else if (type == "input_audio_buffer.append") {
      assert(json.at("audio").get<std::string>().size() == 856);
      {
        std::lock_guard<std::mutex> lock(append_mu);
        append_times.push_back(std::chrono::steady_clock::now());
      }
      if (++appends == 9) {
        ws.sendText(R"({"type":"response.output_audio.delta","delta":"AQI="})");
        ws.sendText(R"({"type":"conversation.item.input_audio_transcription.started"})");
        ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"今天天气"})");
        ws.sendText(R"({"type":"response.output_audio.delta","delta":"AwQ="})");
        ws.sendText(R"({"type":"response.output_audio.done"})");
        ws.sendText(R"({"type":"response.done","usage":{"total_tokens":7}})");
      } else if (appends == 18) {
        ws.sendText(R"({"type":"conversation.item.input_audio_transcription.started"})");
        ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"第二个问题"})");
        ws.sendText(R"({"type":"response.output_audio.delta","delta":"BQY="})");
        ws.sendText(R"({"type":"response.output_audio.done"})");
        ws.sendText(R"({"type":"response.done","usage":{"total_tokens":11}})");
      }
    } else if (type == "response.cancel") {
      ++cancels;
      ws.sendText(R"({"type":"response.canceled"})");
    } else if (type == "session.close") {
      ++closes;
      ws.sendText(R"({"type":"session.closed"})");
    }
  });
  assert(server.listenAndStart());

  xiaoai_plus::config::Config cfg;
  char usage_root[] = "/tmp/xiaoai-client-usage-XXXXXX";
  assert(mkdtemp(usage_root));
  cfg.conversation.usage_dir = std::string(usage_root) + "/usage";
  cfg.conversation.duplex_mode = "full";
  cfg.realtime.api_key = "fake-offline-key";
  cfg.realtime.preset.ws_url = "ws://127.0.0.1:" + std::to_string(port);
  std::mutex mu;
  std::condition_variable cv;
  std::vector<uint8_t> heard;
  std::atomic<int> text_events{0};
  int chats = 0;
  int asr = 0;
  int closed = 0;
  int started_speaking = 0;
  int stopped_speaking = 0;
  Client::Callbacks callbacks;
  callbacks.on_text_event = [&](const auto&) { ++text_events; };
  callbacks.on_audio = [&](const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lock(mu);
    heard.insert(heard.end(), bytes.begin(), bytes.end());
    cv.notify_all();
  };
  callbacks.on_set_ai_speaking = [&](bool value) {
    std::lock_guard<std::mutex> lock(mu);
    value ? ++started_speaking : ++stopped_speaking;
  };
  callbacks.on_asr_final = [&](const std::string& text) {
    assert(text == "今天天气" || text == "第二个问题");
    std::lock_guard<std::mutex> lock(mu);
    ++asr;
  };
  callbacks.on_user_activity = [] {};
  callbacks.on_chat_ended = [&] {
    std::lock_guard<std::mutex> lock(mu);
    ++chats;
    cv.notify_all();
  };
  callbacks.on_session_closed = [&](const std::string&) {
    std::lock_guard<std::mutex> lock(mu);
    ++closed;
  };

  Client client(cfg, callbacks);
  assert(client.Start());
  assert(client.StartSession(3s));
  assert(saw_key && !saw_binary);
  assert(client.SendSayHello());
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 3s, [&] { return chats == 1 && heard.size() == 4; }));
    assert((heard == std::vector<uint8_t>{1, 2, 3, 4}));
    assert(asr == 1 && started_speaking == 1 && stopped_speaking >= 1);
  }
  assert(appends == 9);
  assert(session_creates == 1 && connections == 1 && closes == 0);
  // response.done ends one turn, not the WebSocket session. Send a second user
  // turn on the same connection, with no additional KWS or session.create.
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 3s, [&] { return chats == 2 && heard.size() == 6; }));
    assert((heard == std::vector<uint8_t>{1, 2, 3, 4, 5, 6}));
    assert(asr == 2 && started_speaking == 2 && stopped_speaking >= 2);
  }
  assert(appends == 18 && session_creates == 1 && connections == 1 && closes == 0);
  {
    std::lock_guard<std::mutex> lock(append_mu);
    assert(append_times.size() == 18);
    assert(append_times.back() - append_times.front() >= 120ms);
    assert(append_times[8] - append_times[0] >= 120ms);
    assert(append_times[17] - append_times[9] >= 120ms);
  }
  assert(cancels == 1);
  assert(text_events == 0); // Default-off keeps callback/persistence behavior unchanged.
  xiaoai_plus::config::Config limited = cfg;
  limited.conversation.daily_token_limit = 1;
  Client over_limit(limited, callbacks);
  assert(over_limit.Start());
  const int before_limit = connections;
  assert(!over_limit.StartSession(1s));
  assert(connections == before_limit);
  over_limit.Stop();
  assert(client.FinishSession(2s));
  assert(closes == 1 && closed == 1 && session_creates == 1);

  // Close while recorder output is queued: no late append may turn an ACK into an error.
  assert(client.StartSession(3s));
  assert(client.EnqueueAudio(std::vector<uint8_t>(2880, 0)));
  assert(client.FinishSession(2s));
  assert(closes == 2 && closed == 2);

  // A silent server must time out after exactly one attempt; no hidden retry or extra session.
  ignore_create = true;
  const int before = connections;
  assert(!client.StartSession(350ms));
  assert(connections == before + 1);
  ignore_create = false;
  error_on_create = true;
  for (int i = 0; i < 3; ++i) {
    const int count = connections;
    assert(!client.StartSession(2s));
    assert(connections == count + 1);
    for (int j = 0; j < 20 && !server.getClients().empty(); ++j)
      std::this_thread::sleep_for(25ms);
    assert(server.getClients().empty());
  }
  error_on_create = false;
  drop_on_create = true;
  const int count = connections;
  assert(!client.StartSession(2s));
  std::this_thread::sleep_for(1200ms);
  assert(connections == count + 1);  // IXWebSocket auto-reconnect must be disabled.
  client.Stop();

  // The chargeable first-trial mode permits one connection attempt per process,
  // including an unsuccessful first attempt; later local wakeups cannot redial.
  drop_on_create = false;
  Client one_trial(cfg, callbacks, true);
  assert(one_trial.Start());
  const int trial_before = connections;
  assert(one_trial.StartSession(2s));
  assert(connections == trial_before + 1);
  assert(one_trial.FinishSession(2s));
  assert(!one_trial.StartSession(2s));
  assert(connections == trial_before + 1);
  one_trial.Stop();

  ignore_create = true;
  Client failed_trial(cfg, callbacks, true);
  assert(failed_trial.Start());
  const int failed_before = connections;
  assert(!failed_trial.StartSession(350ms));
  assert(connections == failed_before + 1);
  ignore_create = false;
  assert(!failed_trial.StartSession(2s));
  assert(connections == failed_before + 1);
  failed_trial.Stop();
  idle_release_on_hello = true;
  std::string close_reason;
  Client::Callbacks idle_callbacks;
  idle_callbacks.on_session_closed = [&](const std::string& value) {
    std::lock_guard<std::mutex> lock(mu);
    close_reason = value;
    cv.notify_all();
  };
  Client idle_client(cfg, idle_callbacks);
  assert(idle_client.Start() && idle_client.StartSession(2s));
  assert(idle_client.SendSayHello());
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] { return !close_reason.empty(); }));
    assert(close_reason == "server_idle_release");
  }
  assert(idle_client.FinishSession(2s));
  idle_client.Stop();
  idle_release_on_hello = false;
  server_close_on_hello = true;
  close_reason.clear();
  Client closed_client(cfg, idle_callbacks);
  assert(closed_client.Start() && closed_client.StartSession(2s));
  assert(closed_client.SendSayHello());
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 2s, [&] { return !close_reason.empty(); }));
    assert(close_reason == "session_finished");
  }
  assert(closed_client.FinishSession(2s));
  closed_client.Stop();
  server.stop();
  std::filesystem::remove_all(usage_root);
}
