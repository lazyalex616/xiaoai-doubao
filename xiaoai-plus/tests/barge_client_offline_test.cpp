#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include "realtime/client.hpp"
using namespace std::chrono_literals;
using C=xiaoai_plus::realtime::Client;
using B=xiaoai_plus::realtime::BargeInController;
int main() {
  const int port=ix::getFreePort();assert(port>0);
  ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> creates{0},cancels{0},appends{0},unmutes{0},closes{0},hellos{0};
  std::mutex mu;std::condition_variable cv;std::vector<uint8_t> heard;
  int chats=0,exits=0,texts=0,asr=0,closed=0,stops=0,done=0;
  std::vector<std::chrono::steady_clock::time_point> times;
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type!=ix::WebSocketMessageType::Message) return;
    const auto j=nlohmann::json::parse(msg->str);const auto type=j.at("type").get<std::string>();
    if(type=="session.create") {++creates;ws.sendText(R"({"type":"session.created","session":{"id":"local"}})");}
    else if(type=="speech_text_buffer.commit" && (++hellos==1 || hellos>=3)) {
      ws.sendText(R"({"type":"response.created","response_id":"r1"})");
      ws.sendText(R"({"type":"response.output_audio.delta","response_id":"r1","delta":"AQI="})");
    } else if(type=="response.cancel") {
      ++cancels;
      // Neither anonymous cancellation nor late done/farewell releases a fence.
      ws.sendText(R"({"type":"response.canceled"})");
      ws.sendText(R"({"type":"response.output_audio.delta","response_id":"r1","delta":"AwQ="})");
      ws.sendText(R"({"type":"response.output_audio.delta","delta":"BQY="})");
      ws.sendText(R"({"type":"response.output_audio.done","response_id":"r1","status_code":20000002})");
      ws.sendText(R"({"type":"response.output_text.done","response_id":"r1","text":"goodbye"})");
      ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"synthetic stale identity"})");
      ws.sendText(R"({"type":"response.done","response_id":"r1","usage":{"total_tokens":7}})");
      ws.sendText(R"({"type":"response.created","response_id":"r1"})"); // reused, rejected
      ws.sendText(R"({"type":"response.created","response_id":"unseen-late-created"})");
      ws.sendText(R"({"type":"response.output_audio.delta","response_id":"unseen-late-created","delta":"Cww="})");
      ws.sendText(R"({"type":"response.output_audio.done","response_id":"unseen-late-created","status_code":20000002})");
      ws.sendText(R"({"type":"response.output_audio.done","response_id":"r1","status_code":"20000002"})");
      ws.sendText(R"({"type":"response.output_audio.delta","response_id":"r1","delta":"CQo="})");
    } else if(type=="speech_text_buffer.commit" && hellos==2) {
      ws.sendText(R"({"type":"response.created","response_id":"r2"})");
      ws.sendText(R"({"type":"response.output_audio.delta","response_id":"r2","delta":"Bwg="})");
      ws.sendText(R"({"type":"response.output_audio.done","response_id":"r1","status_code":20000002})");
    } else if(type=="input_audio_unmute.commit") {++unmutes;}
    else if(type=="input_audio_buffer.append") {
      {std::lock_guard<std::mutex> l(mu);times.push_back(std::chrono::steady_clock::now());}
      if(++appends==3) {
        ws.sendText(R"({"type":"response.output_audio.done","response_id":"r2"})");
        ws.sendText(R"({"type":"response.done","response_id":"r2","usage":{"total_tokens":11}})");
      }
      cv.notify_all();
    } else if(type=="session.close") {++closes;ws.sendText(R"({"type":"session.closed"})");}
  });assert(server.listenAndStart());
  char temp[]="/tmp/xiaoai-barge-usage-XXXXXX";assert(mkdtemp(temp));
  xiaoai_plus::config::Config cfg;cfg.conversation.duplex_mode="half";
  cfg.conversation.usage_dir=std::string(temp)+"/usage";
  cfg.realtime.api_key="synthetic-local-only";
  cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);
  C* ptr=nullptr;C::Callbacks callbacks;
  callbacks.on_audio=[&](const auto& b){assert(ptr->SetPlaybackActiveBound(ptr->BargeToken(),true));std::lock_guard<std::mutex> l(mu);heard.insert(heard.end(),b.begin(),b.end());cv.notify_all();};
  callbacks.on_audio_done=[&]{std::lock_guard<std::mutex> l(mu);++done;cv.notify_all();};
  callbacks.on_set_ai_speaking=[&](bool value){if(!value) assert(ptr->SetPlaybackActiveBound(ptr->BargeToken(),false));};
  callbacks.on_chat_ended=[&]{std::lock_guard<std::mutex> l(mu);++chats;cv.notify_all();};
  callbacks.on_exit_intent=[&]{std::lock_guard<std::mutex> l(mu);++exits;};
  callbacks.on_asr_final=[&](const auto&){std::lock_guard<std::mutex> l(mu);++asr;};
  callbacks.on_response_text=[&](const auto& e){assert(e.type=="response.created" || e.type=="response.done");std::lock_guard<std::mutex> l(mu);if(e.type=="response.created")++texts;};
  callbacks.on_session_closed=[&](const auto&){std::lock_guard<std::mutex> l(mu);++closed;cv.notify_all();};
  C client(cfg,callbacks,false,false,true);ptr=&client;client.SetBargeContext(42,1);
  assert(client.Start());assert(client.StartSession(2s));assert(client.SendSayHello());
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return heard.size()==2;}));}
  const auto old=client.BargeToken();const B::Eligibility yes{true,true,false,false,true};
  auto blocked=yes;blocked.native_blocked=true;
  assert(!client.ConfirmBargeIn(old,blocked,[&]{++stops;return true;}));assert(stops==0);
  assert(client.ConfirmBargeIn(old,yes,[&]{
    ++stops;
    // A Player completion callback on another thread can query Client without
    // deadlocking the stop waiter. Pending ingress/grants/hold remain closed.
    std::thread callback([&]{
      const auto pending=client.BargeToken();
      assert(!client.EnqueueAudioBound(pending,std::vector<uint8_t>(640,0)));
      assert(!client.AuthorizeBargeResponse(pending,"too-early"));
      assert(!client.SetPlaybackActiveBound(pending,false,true));
    }); callback.join(); return true;
  }));
  const auto canceled=client.BargeToken();assert(canceled.epoch==old.epoch && canceled.session==old.session);
  assert(!client.ConfirmBargeIn(old,yes,[&]{++stops;return true;}));
  xiaoai_plus::realtime::DailyUsage usage(cfg.conversation.usage_dir);
  for(int i=0;i<100 && usage.ReadToday()!=7;++i) std::this_thread::sleep_for(10ms);
  assert(usage.ReadToday()==7);
  // A fixture-owned grant, not a wire canceled ACK. App has no such proof yet.
  assert(client.AuthorizeBargeResponse(canceled,"r2"));
  assert(client.SendSayHello("synthetic_new_response_boundary"));
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return heard.size()==4;}));assert((heard==std::vector<uint8_t>{1,2,7,8}));assert(chats==0&&exits==0&&asr==0&&done==0&&texts==2);}
  assert(cancels==1&&creates==1&&stops==1);
  const auto current=client.BargeToken();
  assert(!client.SetPlaybackActiveBound(old,false,true));
  assert(!client.EnqueueAudioBound(old,std::vector<uint8_t>(640,0)));
  // Still held by current playback; the old release could not open it.
  assert(client.EnqueueAudioBound(current,std::vector<uint8_t>(640,0)));std::this_thread::sleep_for(80ms);assert(appends==0);
  assert(client.SetPlaybackActiveBound(current,false));
  assert(client.EnqueueAudioBound(current,std::vector<uint8_t>(640,0)));std::this_thread::sleep_for(100ms);assert(appends==0);
  std::this_thread::sleep_for(240ms);assert(client.EnqueueAudioBound(current,std::vector<uint8_t>(1920,0)));
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return chats==1;}));assert(done==1&&exits==0&&asr==0);assert(times.size()==3);for(size_t i=1;i<times.size();++i) assert(times[i]-times[i-1]>=15ms);}
  assert(usage.ReadToday()==18);
  assert(client.FinishSession(2s));assert(closes==1);
  assert(!client.ConfirmBargeIn(current,yes,[]{return true;}));
  client.SetBargeContext(43,2);assert(client.StartSession(2s));
  assert(!client.EnqueueAudioBound(current,std::vector<uint8_t>(640,0)));
  assert(client.BargeToken().epoch==43 && client.BargeToken().session!=current.session);
  assert(client.SendSayHello());
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return heard.size()==6;}));}
  const auto replacing=client.BargeToken();
  assert(!client.ConfirmBargeIn(replacing,yes,[&]{client.SetBargeContext(44,3);return true;}));
  std::this_thread::sleep_for(40ms);assert(cancels==1 && creates==2 && closes==1);
  assert(client.FinishSession(2s));
  client.SetBargeContext(45,4);assert(client.StartSession(2s));assert(client.SendSayHello());
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return heard.size()==8;}));}
  const auto fail_stop=client.BargeToken();
  assert(!client.ConfirmBargeIn(fail_stop,yes,[]{return false;}));
  {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return closed==3;}));}
  assert(cancels==1 && creates==3 && closes==2);
  assert(!client.EnqueueAudioBound(client.BargeToken(),std::vector<uint8_t>(640,0)));
  client.Stop();server.stop();
  std::filesystem::remove_all(temp);
  std::cout<<"PASS scoped_cancel_once old_audio_done_farewell_20000002_dropped new_audio_only\n"
           <<"PASS epoch_session_preserved old_ingress_rejected old_hold_release_rejected\n"
           <<"PASS tail_300ms_and_20ms_pacing retained_usage_old_and_new=18\n"
           <<"PASS cross_session_old_confirmation_rejected pending_stop_callback_no_deadlock context_replacement_no_global_cancel failed_stop_closes_without_cancel_or_unmute scoped_ASR_forwarding_unavailable\n";
}
