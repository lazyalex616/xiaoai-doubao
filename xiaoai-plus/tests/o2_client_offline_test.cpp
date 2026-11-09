#ifdef NDEBUG
#error requires active assertions
#endif
#include <atomic>
#include <cassert>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <ixwebsocket/IXGetFreePort.h>
#include "realtime/client.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using namespace realtime::protocol;
using J=nlohmann::json;
int main() {
  int port=ix::getFreePort();assert(port>0);ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> opens{0},final_asr{0},answers{0},chat_ended{0};
  std::string sid;
  server.setOnClientMessageCallback([&](auto,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type==ix::WebSocketMessageType::Open){++opens;return;}
    if(msg->type!=ix::WebSocketMessageType::Message)return;
    assert(msg->binary);auto f=DecodeFrame({msg->str.begin(),msg->str.end()});
    if(f.session_id)sid=*f.session_id;
    auto send=[&](int id,J body=J::object()) {
      auto b=BuildJsonFrame(MessageType::kFullServerResponse,static_cast<EventId>(id),body,sid);
      assert(ws.sendBinary(std::string(b.begin(),b.end())).success);
    };
    if(f.event_id==EventId::kStartConnection)send(50);
    if(f.event_id==EventId::kStartSession) {
      assert(f.JsonPayload()["dialog"]["system_role"].get<std::string>().find("Volume requests: say only '好的'")!=std::string::npos);
      assert(f.JsonPayload()["asr"]["extra"]["end_smooth_window_ms"]==2500);send(150);
    }
    if(f.event_id==EventId::kSayHello) {
      send(450,{{"question_id","q1"}});
      send(451,{{"results",J::array({{{"text","我是 Amy"},{"is_interim",false}}})}});send(459);
      send(550,{{"question_id","q1"},{"reply_id","r1"},{"content","Hi Amy"}});
      send(559,{{"question_id","q1"},{"reply_id","r1"}});
    }
    if(f.event_id==EventId::kFinishSession) {
      // FinishSession(0) has already retired dialogue authority. This usage
      // still belongs in the ledger and must not trigger live turn callbacks.
      send(154,{{"usage",{{"input_text_tokens",1},{"input_audio_tokens",2},{"cached_text_tokens",0},
          {"cached_audio_tokens",0},{"output_text_tokens",3},{"output_audio_tokens",4}}}});
      std::this_thread::sleep_for(80ms);send(152);
    }
  });assert(server.listenAndStart());
  char dir[]="/tmp/o2-client-XXXXXX";assert(mkdtemp(dir));
  config::Config cfg;cfg.realtime.protocol="o2";cfg.realtime.api_key="loopback-only";cfg.memory.enabled=true;
  cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);cfg.conversation.usage_dir=dir;
  realtime::Client::Callbacks cb;
  cb.on_asr_final=[&](const auto& s){assert(s=="我是 Amy");++final_asr;};
  cb.on_text_event=[&](const auto& e){if(e.type=="response.output_text.done"){assert(e.text=="Hi Amy");++answers;}};
  cb.on_chat_ended=[&]{++chat_ended;};
  realtime::Client client(cfg,cb);assert(client.Start());assert(client.StartSession(2s));
  assert(client.SendSayHello("identity"));
  for(int i=0;i<100 && answers<1;++i)std::this_thread::sleep_for(10ms);
  assert(final_asr==1 && answers==1);assert(client.FinishSession(0ms));
  // No artificial sleep: StartSession joins the retired socket before reading its ledger.
  assert(client.StartSession(2s));assert(opens==2 && chat_ended==0);
  realtime::DailyUsage usage(dir);assert(usage.ReadToday()==10);
  client.Stop();server.stop();std::filesystem::remove_all(dir);
}
