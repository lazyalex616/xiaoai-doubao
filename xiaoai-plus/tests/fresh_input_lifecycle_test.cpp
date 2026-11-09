#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <algorithm>
#include <ctime>
#include <atomic>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "memory/service.hpp"
#include "realtime/client.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using J=memory::Json;
using E=realtime::TransportEnvelope;
using C=realtime::Client;
template<class F> memory::ResumeResult Reply(F f) {
  std::promise<memory::ResumeResult> p;auto done=p.get_future();
  assert(f([&](auto r){p.set_value(std::move(r));}));
  assert(done.wait_for(3s)==std::future_status::ready);return done.get();
}
template<class F> void Wait(F f) {
  for(int i=0;i<300 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());
}
uint64_t NowNs() {return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int main() {
  char tmp[]="/tmp/fresh-input-XXXXXX";assert(mkdtemp(tmp));const std::string dir=tmp;
  config::Memory mc;mc.enabled=true;mc.cache_dir=dir+"/memory";std::atomic<int> actions{0};
  memory::Service svc(mc,[](const std::string& path,const J&)->memory::Reply {
    if(path=="/device/snapshot")return {200,{{"generation",9},{"expires_at",std::time(nullptr)+3600},
      {"children",J::array({J{{"id","amy"},{"version",4},{"profile",{{"name","Amy"},{"aliases",J::array({"Amy"})}}},{"summaries",J::array()}}})}}};
    return {200,J::object()};
  },[&](memory::Action){++actions;});
  svc.Start();Wait([&]{return svc.cache_revision()>0;});assert(svc.Begin(42));Wait([&]{return actions==1;});
  realtime::duplex::Session parse;parse.Create("fixture","fixture");parse.Receive(R"({"type":"session.created","session":{"id":"parser"}})");
  assert(svc.Event(42,parse.Receive(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"identify","item_id":"identify","transcript":"I am Amy"})")));
  Wait([&]{return actions==2;});assert(svc.Activate(42));
  auto inspect=[&]{return Reply([&](auto cb){return svc.InspectTransportResume(42,cb);});};
  assert(inspect().approved);
  const int port=ix::getFreePort();ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> creates{0},events{0},audio{0},audio_done{0};std::atomic<bool> emit_output{false};std::mutex mu;std::vector<J> wire;
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type!=ix::WebSocketMessageType::Message)return;auto j=J::parse(msg->str);
    {std::lock_guard<std::mutex> lock(mu);wire.push_back(j);}
    if(j.at("type")=="session.create")ws.sendText(J{{"type","session.created"},{"session",{{"id","session-"+std::to_string(++creates)}}}}.dump());
    if(j.at("type")=="input_audio_buffer.append" && emit_output.exchange(false))ws.sendText(R"({"type":"response.output_audio.delta","event_id":"ws-output","question_id":"q3","response_id":"r3","delta":"AQI="})");
  });assert(server.listenAndStart());
  auto count=[&](const std::string& type){std::lock_guard<std::mutex> lock(mu);return std::count_if(wire.begin(),wire.end(),[&](const auto& j){return j.at("type")==type;});};
  auto clear=[&]{std::lock_guard<std::mutex> lock(mu);wire.clear();};
  C::Callbacks callbacks;callbacks.on_transport_event=[&](const E& e,const auto& event){++events;if(event.type=="response.output_audio.delta")++audio;if(event.type=="response.output_audio.done")++audio_done;assert(svc.Event(e,event));};
  config::Config cfg;cfg.realtime.api_key="synthetic-loopback-only";cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);cfg.conversation.usage_dir=dir+"/usage";
  const realtime::FreshTransportOwner::Proof proof{true,true,true,true,true,false};
  auto prepare=[&](uint64_t n){const auto id=inspect().identity;return Reply([&](auto cb){return svc.PrepareTransportResume(id,n,77,n,cb);});};
  auto grant=[&](C& c,const E& e){auto r=Reply([&](auto cb){return svc.ActivateTransportResume(e,cb);});assert(r.approved);assert(c.GrantFreshInput(r.permit,"session-"+std::to_string(creates.load()),77,proof));};
  auto open=[&](C& c,uint64_t n){auto r=prepare(n);assert(r.approved);assert(c.PrepareFreshTransport(r.envelope));assert(c.Start());assert(c.StartSession(2s,r.instructions));grant(c,r.envelope);Wait([&]{return count("input_audio_unmute.commit")==1;});return r.envelope;};
  auto retire=[&](C& c,E e){assert(c.RetireFreshTransport(e,[]{return true;}));assert(svc.RetireTransportResume(e));c.Stop();};
  realtime::DailyUsage usage(cfg.conversation.usage_dir);

  C a(cfg,callbacks,false,false,false,true);E ea=open(a,1);clear();
  assert(!a.EndFreshInput(ea)); // Empty input cannot manufacture an endpoint.
  std::vector<uint8_t> pcm(960,0x35);assert(a.EnqueueFreshAudio(ea,pcm));
  assert(a.EndFreshInput(ea));assert(!a.EndFreshInput(ea));assert(!a.EnqueueFreshAudio(ea,std::vector<uint8_t>(640,0)));
  Wait([&]{return count("input_audio_mute.commit")==1;});
  {std::lock_guard<std::mutex> lock(mu);assert(wire.size()==4);
    assert(wire[0]["type"]=="input_audio_buffer.append" && wire[1]["type"]=="input_audio_buffer.append");
    assert(wire[2]["type"]=="input_audio_buffer.commit" && wire[3]["type"]=="input_audio_mute.commit");
    for(size_t i=0;i<2;++i){const auto decoded=parse.Receive(J{{"type","response.output_audio.delta"},{"delta",wire[i]["audio"]}}.dump()).audio;
      assert(decoded.size()==640);for(size_t k=0;k<640;++k)assert(decoded[k]==((i==0 || k<320)?0x35:0));}
  }
  const int before=events;
  a.HandleTextBound(ea,R"({"type":"response.output_audio.delta","event_id":"a1","question_id":"q1","response_id":"r1","delta":"AQI="})");
  a.HandleTextBound(ea,R"({"type":"response.done","usage":{"total_tokens":10}})");
  assert(events==before+1 && usage.ReadToday()==10); // Billing never calls output completion.
  assert(!a.ConfirmFreshPlaybackDrained(ea));
  a.HandleTextBound(ea,R"({"type":"response.output_audio.delta","event_id":"tail","question_id":"q1","response_id":"r1","delta":"AwQ="})");assert(audio==2);
  a.HandleTextBound(ea,R"({"type":"response.output_audio.done"})");assert(audio_done==0);
  a.HandleTextBound(ea,R"({"type":"response.output_audio.done","event_id":"done","question_id":"q1","response_id":"r1"})");assert(audio_done==1);
  assert(!a.ConfirmFreshPlaybackDrained(E(42,1,9,4,99,99,77,99)));
  assert(a.ConfirmFreshPlaybackDrained(ea));assert(!a.ConfirmFreshPlaybackDrained(ea));
  a.HandleTextBound(ea,R"({"type":"response.output_audio.delta","event_id":"after-drain","question_id":"q1","response_id":"r1","delta":"AwQ="})");assert(audio==2);
  retire(a,ea);assert(count("response.cancel")==1 && count("session.close")==1);

  clear();C b(cfg,callbacks,false,false,false,true);assert(b.InheritRetiredUsage(a));E eb=open(b,2);clear();
  C::CapturedInput captured;captured.origin={42,11,12,13,14,77};captured.transfer_input=eb.input();captured.first_read_ns=captured.delivered_ns=NowNs();
  auto invalid=captured;invalid.first_read_ns=NowNs()-4000000000ULL;assert(!b.EnqueueFreshCapturedAudio(eb,std::vector<uint8_t>(640,0),invalid));
  assert(b.EnqueueFreshCapturedAudio(eb,std::vector<uint8_t>(640,0x22),captured));
  assert(!b.EnqueueFreshCapturedAudio(eb,std::vector<uint8_t>(640,0),captured)); // No replay.
  Wait([&]{return count("input_audio_buffer.append")==1;});
  assert(b.EnqueueFreshAudio(eb,std::vector<uint8_t>(31680,0x55)));Wait([&]{return count("input_audio_buffer.append")>=2;});
  const auto pause_start=std::chrono::steady_clock::now();
  assert(!b.PauseFreshInput(ea,"wrong origin"));
  assert(b.PauseFreshInput(eb,"fixture output starts"));assert(!b.PauseFreshInput(eb,"duplicate"));
  assert(std::chrono::steady_clock::now()-pause_start<100ms); // Callback does not join or wait pacing.
  assert(!b.EndFreshInput(eb));assert(!b.EnqueueFreshAudio(eb,std::vector<uint8_t>(640,0)));
  captured.first_sample=320;captured.first_read_ns=captured.delivered_ns=NowNs();assert(!b.EnqueueFreshCapturedAudio(eb,std::vector<uint8_t>(640,0),captured));
  Wait([&]{return count("input_audio_mute.commit")==1;});const auto paused_count=count("input_audio_buffer.append");
  std::this_thread::sleep_for(80ms);assert(count("input_audio_buffer.append")==paused_count);
  assert(count("input_audio_buffer.commit")==0 && paused_count<5); // Queued chunk is discarded promptly, no endpoint fiction.
  b.HandleTextBound(eb,R"({"type":"response.output_audio.delta","event_id":"b1","question_id":"q2","response_id":"r2","delta":"AQI="})");assert(audio==3);
  const int retired_events=events;retire(b,eb);assert(b.UnsettledFreshUsage()==1);

  clear();C c(cfg,callbacks,false,false,false,true);assert(c.InheritRetiredUsage(b));assert(c.UnsettledFreshUsage()==1);E ec=open(c,3); // No done never introduces permanent UNKNOWN.
  const int next_created=events;emit_output=true;assert(c.EnqueueFreshAudio(ec,std::vector<uint8_t>(640,0)));
  Wait([&]{return audio==4;});assert(!c.EndFreshInput(ec));assert(!c.EnqueueFreshAudio(ec,std::vector<uint8_t>(640,0)));
  Wait([&]{return count("input_audio_mute.commit")==2;});
  b.HandleTextBound(eb,R"({"type":"response.output_text.done","event_id":"old","question_id":"q2","response_id":"r2","text":"fixture retired"})");
  b.HandleTextBound(eb,R"({"type":"response.done","usage":{}})");assert(c.UnsettledFreshUsage()==2);
  c.HandleTextBound(ec,R"({"type":"response.done","usage":{}})"); // Independent current UNKNOWN must survive old recovery.
  const int settled_events=events;
  c.HandleTextBound(eb,R"({"type":"response.done","usage":{"total_tokens":3}})");
  b.HandleTextBound(eb,R"({"type":"response.done","usage":{"total_tokens":3}})");assert(usage.ReadToday()==13 && c.UnsettledFreshUsage()==1 && events==settled_events && retired_events+2==settled_events && next_created+1==settled_events);
  c.HandleTextBound(ec,R"({"type":"response.done","usage":{}})");retire(c,ec);
  clear();C d(cfg,callbacks,false,false,false,true);assert(d.InheritRetiredUsage(c));auto fourth=prepare(4);assert(fourth.approved);
  assert(!d.PrepareFreshTransport(fourth.envelope));assert(d.UnsettledFreshUsage()==1);
  c.HandleTextBound(ec,R"({"type":"response.done","usage":{"total_tokens":2}})");assert(usage.ReadToday()==15 && d.UnsettledFreshUsage()==0);
  assert(d.PrepareFreshTransport(fourth.envelope));assert(d.Start());assert(d.StartSession(2s,fourth.instructions));grant(d,fourth.envelope);
  captured.transfer_input=fourth.envelope.input();captured.first_sample=960;captured.first_read_ns=captured.delivered_ns=NowNs();
  assert(d.EnqueueFreshCapturedAudio(fourth.envelope,std::vector<uint8_t>(640,0x66),captured));
  captured.first_sample=1280;captured.first_read_ns=captured.delivered_ns=NowNs();
  assert(d.EnqueueFreshCapturedAudio(fourth.envelope,std::vector<uint8_t>(640,0x77),captured));
  assert(d.EndFreshInput(fourth.envelope));assert(!d.EnqueueFreshCapturedAudio(fourth.envelope,std::vector<uint8_t>(640,0),captured));
  Wait([&]{return count("input_audio_buffer.commit")==1 && count("input_audio_mute.commit")==2;});
  d.HandleTextBound(fourth.envelope,R"({"type":"response.done","usage":{"total_tokens":5}})");retire(d,fourth.envelope);
  config::Config limited=cfg;limited.conversation.daily_token_limit=20;C budget(limited,{},false,false,false,true);
  assert(!budget.InheritRetiredUsage(d));assert(budget.PrepareFreshTransport(E(42,1,9,4,100,100,77,100)));assert(budget.Start());assert(!budget.StartSession(20ms));budget.Stop();
  assert(usage.ReadToday()==20);
  clear();C storage(cfg,callbacks,false,false,false,true);assert(storage.InheritRetiredUsage(d));E ef=open(storage,5);
  assert(storage.EnqueueFreshAudio(ef,std::vector<uint8_t>(640,0)));retire(storage,ef);assert(storage.UnsettledFreshUsage()==1);
  assert(chmod(cfg.conversation.usage_dir.c_str(),0755)==0);
  storage.HandleTextBound(ef,R"({"type":"response.done","usage":{"total_tokens":7}})"); // Real private-ledger refusal.
  assert(chmod(cfg.conversation.usage_dir.c_str(),0700)==0);
  storage.HandleTextBound(ef,R"({"type":"response.done","usage":{"total_tokens":7}})");assert(usage.ReadToday()==20 && storage.UnsettledFreshUsage()==1);
  C storage_successor(cfg,{},false,false,false,true);assert(storage_successor.InheritRetiredUsage(storage));auto fs=prepare(6);assert(fs.approved);
  assert(!storage_successor.PrepareFreshTransport(fs.envelope)); // Original storage UNKNOWN persists, no ambiguous retry debit.
  svc.Stop();server.stop();std::filesystem::remove_all(dir);
  std::cout<<"PASS real sender accepted PCM drain tail <=1 commit then mute; no empty/repeated/paused endpoint\n"
           <<"PASS captured source/read-age/replay checks and async Pause stops queue promptly; output lease preserved\n"
           <<"PASS response.done billing only, legal late audio tail survives until audio.done and explicit Player drain\n"
           <<"PASS new Client per next input, retired unknown recovery/dedup shared, no done continues, daily budget unchanged\n"
           <<"PASS real ledger failure UNKNOWN inherited; no repeated debit after uncertain write failure\n";
}
