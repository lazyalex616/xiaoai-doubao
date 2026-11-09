#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <iostream>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "memory/service.hpp"
#include "realtime/client.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using J=memory::Json;
using E=realtime::TransportEnvelope;
template<class F> void Wait(F f) {for(int i=0;i<500 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());}
template<class F> memory::ResumeResult Reply(F f) {
  std::promise<memory::ResumeResult> p;auto future=p.get_future();assert(f([&](auto r){p.set_value(std::move(r));}));
  assert(future.wait_for(3s)==std::future_status::ready);return future.get();
}
J Read(const std::string& path){std::ifstream f(path);J j;f>>j;return j;}
realtime::duplex::Event Asr(const std::string& text,const std::string& event="name",const std::string& item="input") {
  realtime::duplex::Session p;p.Create("local","local");p.Receive(R"({"type":"session.created","session":{"id":"local"}})");
  return p.Receive(J{{"type","conversation.item.input_audio_transcription.completed"},{"event_id",event},{"item_id",item},{"transcript",text}}.dump());
}
J Snapshot(int64_t expiry) {
  J n={{"id","amy"},{"version",4},{"profile",{{"name","Amy"},{"aliases",J::array({"Amy"})},{"secret","PRIVATE_AMY_SECRET"}}},
       {"summaries",J::array({J{{"recap","AMY_INITIAL_HISTORY"}}})}};
  J k={{"id","ben"},{"version",4},{"profile",{{"name","Ben"},{"aliases",J::array({"Ben"})}}},
       {"summaries",J::array({J{{"recap","BEN_HISTORY"}}})}};
  return {{"generation",9},{"expires_at",expiry},{"children",J::array({n,k})}};
}
int main() {
  char tmp[]="/tmp/xz-resume-snapshot-XXXXXX";assert(mkdtemp(tmp));const std::string dir=tmp;
  config::Memory mc;mc.enabled=true;mc.cache_dir=dir+"/memory";
  std::mutex facts_mu;J facts=Snapshot(std::time(nullptr)+3600);
  std::atomic<int> actions{0};std::atomic<bool> block_fetch{false},fetch_waiting{false},release_fetch{false};
  memory::Service svc(mc,[&](const std::string& path,const J&)->memory::Reply {
    if(path!="/device/snapshot")return {200,J::object()};
    J response;{std::lock_guard<std::mutex> l(facts_mu);response=facts;}
    if(block_fetch.exchange(false)){fetch_waiting=true;Wait([&]{return release_fetch.load();});}
    return {200,std::move(response)};
  },[&](memory::Action a){++actions;if(a.restart)assert(svc.Activate(a.epoch));});
  svc.Start();Wait([&]{return svc.cache_revision()>0;});assert(svc.Begin(42));Wait([&]{return actions==1;});
  assert(svc.Event(42,Asr("I am Amy")));Wait([&]{return actions==2;});
  auto inspect=[&]{return Reply([&](auto cb){return svc.InspectTransportResume(42,cb);});};
  auto prepare=[&](uint64_t n){const auto i=inspect();assert(i.approved);return Reply([&](auto cb){return svc.PrepareTransportResume(i.identity,n,1,n,cb);});};
  auto activate=[&](E e){return Reply([&](auto cb){return svc.ActivateTransportResume(e,cb);});};
  auto fetch=[&]{const auto count=svc.snapshot_receipts();assert(svc.Prefetch());Wait([&]{return svc.snapshot_receipts()>count;});};
  const auto original=inspect();assert(original.identity.child_id=="amy");
  auto first=prepare(1);const auto revision=svc.cache_revision();fetch();assert(svc.cache_revision()==revision);
  {std::lock_guard<std::mutex> l(facts_mu);facts["expires_at"]=original.identity.expires_at+600;}
  fetch();auto first_permit=activate(first.envelope);assert(first_permit.approved && first_permit.permit.valid());
  assert(first_permit.identity.expires_at==original.identity.expires_at); // Fetch cannot renew an admitted lease.
  assert(svc.RetireTransportResume(first.envelope));
  auto pending=prepare(2);
  {std::lock_guard<std::mutex> l(facts_mu);facts["children"][0]["summaries"][0]["recap"]="AMY_NEW_SUMMARY";}
  fetch();assert(!activate(pending.envelope).approved);assert(inspect().instructions.find("AMY_NEW_SUMMARY")!=std::string::npos);

  const int port=ix::getFreePort();ix::WebSocketServer server(port,"127.0.0.1");std::atomic<int> sessions{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type!=ix::WebSocketMessageType::Message)return;const auto j=J::parse(msg->str);
    if(j.at("type")=="session.create")ws.sendText(J{{"type","session.created"},{"session",{{"id","local-"+std::to_string(++sessions)}}}}.dump());
  });assert(server.listenAndStart());
  config::Config cfg;cfg.realtime.api_key="loopback-only";cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);
  cfg.conversation.usage_dir=dir+"/usage";std::atomic<int> audio{0};realtime::Client::Callbacks cb;
  cb.on_transport_event=[&](const E& e,const realtime::duplex::Event& v){if(v.type=="response.output_audio.delta")++audio;assert(svc.Event(e,v));};
  realtime::Client client(cfg,cb,false,false,false,true);auto current=prepare(3);assert(current.approved);
  assert(client.PrepareFreshTransport(current.envelope));assert(client.Start());assert(client.StartSession(2s,current.instructions));
  const auto grant=activate(current.envelope);assert(grant.approved);
  realtime::FreshTransportOwner::Proof proof{true,true,true,true,true,false};
  assert(client.GrantFreshInput(grant.permit,"local-1",1,proof));
  client.HandleTextBound(current.envelope,R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"question","item_id":"input","transcript":"Tell me a dinosaur joke"})");
  Wait([&]{return Read(mc.cache_dir+"/active.json")["turns"].size()==1;});
  client.HandleTextBound(current.envelope,R"({"type":"response.output_audio.delta","event_id":"a1","response_id":"r","question_id":"q","delta":"AQI="})");
  {std::lock_guard<std::mutex> l(facts_mu);facts["children"][0]["version"]=5;facts["children"][0]["profile"]["grade"]="3";facts["children"][0]["summaries"][0]["recap"]="AMY_LATEST_SUMMARY";}
  fetch();assert(grant.permit.valid());
  client.HandleTextBound(current.envelope,R"({"type":"response.output_text.done","event_id":"text-tail","response_id":"r","question_id":"q","text":"The dinosaur joke has a legal tail"})");
  client.HandleTextBound(current.envelope,R"({"type":"response.output_audio.delta","event_id":"a2","response_id":"r","question_id":"q","delta":"AwQ="})");
  Wait([&]{return Read(mc.cache_dir+"/active.json")["turns"].size()==2;});
  const auto old_session=Read(mc.cache_dir+"/active.json");assert(old_session["profile_version"]==4 && audio==2);
  client.HandleTextBound(current.envelope,R"({"type":"response.output_audio.done","event_id":"audio-done","response_id":"r","question_id":"q"})");
  assert(client.ConfirmFreshPlaybackDrained(current.envelope));assert(svc.PlaybackDrained(current.envelope));
  const auto next=inspect();assert(next.approved && next.identity.profile_version==5 && next.instructions.find("AMY_LATEST_SUMMARY")!=std::string::npos);
  assert(!grant.permit.valid());const auto new_session=Read(mc.cache_dir+"/active.json");
  assert(new_session["session_id"]!=old_session["session_id"] && new_session["child_id"]=="amy" && new_session["profile_version"]==5);
  assert(client.RetireFreshTransport(current.envelope,[]{return true;}));client.Stop();

  // A fetch captured for Amy returns after the real same-epoch identity switch.
  auto bound=prepare(4);auto live=activate(bound.envelope);assert(live.approved);
  {std::lock_guard<std::mutex> l(facts_mu);facts["generation"]=999;facts["children"][0]["summaries"][0]["recap"]="STALE_FETCH_MUST_NOT_APPLY";}
  const auto stale_receipts=svc.snapshot_receipts();block_fetch=true;assert(svc.Prefetch());Wait([&]{return fetch_waiting.load();});
  assert(svc.Event(bound.envelope,Asr("I am Ben","switch")));Wait([&]{return actions==3;});
  assert(!live.permit.valid());
  {std::lock_guard<std::mutex> l(facts_mu);facts=Snapshot(std::time(nullptr)+3600);}
  release_fetch=true;Wait([&]{return svc.snapshot_receipts()>stale_receipts;});
  Wait([&]{return inspect().approved && inspect().identity.child_id=="ben";});
  assert(Read(mc.cache_dir+"/snapshot.json")["generation"]==9);
  assert(svc.Event(bound.envelope,Asr("I am Amy","stale-old-actor")));assert(inspect().identity.child_id=="ben");
  auto ben=prepare(5);auto np=activate(ben.envelope);assert(np.approved);
  {std::lock_guard<std::mutex> l(facts_mu);facts["children"].erase(1);}
  fetch();assert(!np.permit.valid());Wait([&]{return actions>=4;});assert(inspect().identity.kind=="unknown");
  assert(svc.End(42));svc.Stop();server.stop();

  // Unknown has an independent revision and no previous-child/adult instruction.
  for(const std::string name:{"Amy","Ben","我是路人甲","unregistered opaque"}) {
    config::Memory uc=mc;uc.cache_dir=dir+"/unknown-"+std::to_string(actions.load());
    std::atomic<int> ua{0};memory::Service unknown(uc,[&](const std::string& path,const J&)->memory::Reply {
      if(path=="/device/snapshot")return {200,Snapshot(std::time(nullptr)+3600)};
      if(path=="/device/identity")return {200,{{"result",name=="我是路人甲"?"adult":"evil"}}};
      return {200,J::object()};
    },[&](memory::Action a){++ua;if(a.restart)assert(unknown.Activate(a.epoch));});
    unknown.Start();Wait([&]{return unknown.cache_revision()>0;});assert(unknown.Begin(84));Wait([&]{return ua==1;});
    auto u=Reply([&](auto cb){return unknown.InspectTransportResume(84,cb);});assert(u.approved && u.identity.kind=="unknown");
    auto prepared=Reply([&](auto cb){return unknown.PrepareTransportResume(u.identity,1,7,1,cb);});
    assert(prepared.approved && prepared.envelope.identity()>u.identity.revision && prepared.envelope.profile()==-1);
    assert(prepared.instructions.find("AMY_INITIAL_HISTORY")==std::string::npos && prepared.instructions.find("BEN_HISTORY")==std::string::npos);
    auto permit=Reply([&](auto cb){return unknown.ActivateTransportResume(prepared.envelope,cb);});assert(permit.approved);
    assert(unknown.Event(prepared.envelope,Asr(name)));Wait([&]{return ua==2;});
    // Like the App after playback, retire the unknown transport before Activate.
    assert(unknown.RetireTransportResume(prepared.envelope));assert(unknown.Activate(84));
    Wait([&]{return !permit.permit.valid();});
    auto identified=Reply([&](auto cb){return unknown.InspectTransportResume(84,cb);});
    assert(!permit.permit.valid());
    if(name=="Amy" || name=="Ben")assert(identified.approved && identified.identity.child_id==(name=="Amy"?"amy":"ben"));
    else {
      assert(identified.approved && identified.identity.kind=="unknown");
      auto retry=Reply([&](auto cb){return unknown.PrepareTransportResume(identified.identity,2,7,2,cb);});
      assert(retry.approved);
      auto retry_permit=Reply([&](auto cb){return unknown.ActivateTransportResume(retry.envelope,cb);});
      assert(retry_permit.approved);
      assert(unknown.Event(retry.envelope,Asr(name,"name-retry","input-retry")));Wait([&]{return ua==3;});
      assert(unknown.RetireTransportResume(retry.envelope));assert(unknown.Activate(84));
      Wait([&]{return !retry_permit.permit.valid();});
      identified=Reply([&](auto cb){return unknown.InspectTransportResume(84,cb);});
      assert(identified.approved && identified.identity.kind=="guest");
    }
    if(name!="Amy")assert(identified.instructions.find("AMY_INITIAL_HISTORY")==std::string::npos);
    unknown.Stop();++actions;
  }
  // A refreshed cache expiry cannot renew the already Active immutable permit.
  config::Memory ec=mc;ec.cache_dir=dir+"/expiry";int64_t original_expiry=std::time(nullptr)+3600;
  std::atomic<int64_t> refreshed{original_expiry};std::atomic<int> ea{0};
  memory::Service expires(ec,[&](const std::string& path,const J&)->memory::Reply {
    return path=="/device/snapshot"?memory::Reply{200,Snapshot(refreshed)}:memory::Reply{200,J::object()};
  },[&](memory::Action a){++ea;if(a.restart)assert(expires.Activate(a.epoch));});
  expires.Start();Wait([&]{return expires.cache_revision()>0;});assert(expires.Begin(1));Wait([&]{return ea==1;});
  assert(expires.Event(1,Asr("Amy")));Wait([&]{return ea==2;});
  // Choose the short ORIGINAL expiry after startup/identity I/O, not before it.
  original_expiry=std::time(nullptr)+5;refreshed=original_expiry;
  auto short_receipt=expires.snapshot_receipts();assert(expires.Prefetch());Wait([&]{return expires.snapshot_receipts()>short_receipt;});
  auto e=Reply([&](auto cb){return expires.InspectTransportResume(1,cb);});
  auto ep=Reply([&](auto cb){return expires.PrepareTransportResume(e.identity,1,1,1,cb);});
  auto permit=Reply([&](auto cb){return expires.ActivateTransportResume(ep.envelope,cb);});assert(permit.approved);
  refreshed=original_expiry+600;const auto receipts=expires.snapshot_receipts();assert(expires.Prefetch());Wait([&]{return expires.snapshot_receipts()>receipts;});
  assert(permit.permit.valid());Wait([&]{return std::time(nullptr)>=original_expiry;});assert(!permit.permit.valid());
  Wait([&]{return ea==3;});expires.Stop();std::filesystem::remove_all(dir);
  std::cout<<"PASS real Service/Client/loopback Prepared vs Active snapshot summary/profile tail, latest next input, actor/stale fetch/delete, unknown names and immutable original expiry; fixture only\n";
}
