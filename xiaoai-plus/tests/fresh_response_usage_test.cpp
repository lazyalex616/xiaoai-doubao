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
  auto open=[&](C& c,uint64_t n){clear();auto r=prepare(n);assert(r.approved);assert(c.PrepareFreshTransport(r.envelope));assert(c.Start());assert(c.StartSession(2s,r.instructions));grant(c,r.envelope);Wait([&]{return count("input_audio_unmute.commit")==1;});return r.envelope;};
  auto retire=[&](C& c,E e){assert(c.RetireFreshTransport(e,[]{return true;}));assert(svc.RetireTransportResume(e));c.Stop();};

  // Synthetic contract fixtures, actual Service thread / loopback Client methods.
  // They prove local accounting isolation, not provider event order or acoustics.
  uint64_t next_input=1;
  auto begin=[&](C& c){return open(c,next_input++);};
  auto delta=[](std::string r,std::string q){return J{{"type","response.output_audio.delta"},{"event_id","observed-"+r},{"response_id",r},{"question_id",q},{"delta","AQI="}}.dump();};
  auto done=[](std::string r,std::optional<uint64_t> total,std::string q=""){
    J j{{"type","response.done"},{"event_id","bill-"+r}};
    if(!r.empty())j["response_id"]=r;if(!q.empty())j["question_id"]=q;
    if(total)j["usage"]={{"total_tokens",*total}};return j.dump();
  };
  auto pcm=[&](C& c,E e){assert(c.EnqueueFreshAudio(e,std::vector<uint8_t>(640,0)));Wait([&]{return count("input_audio_buffer.append")==1;});};
  auto successor=[&](C& c,C& old,bool allowed){assert(c.InheritRetiredUsage(old));auto r=prepare(next_input++);assert(r.approved);assert(c.PrepareFreshTransport(r.envelope)==allowed);
    if(allowed){assert(c.RetireFreshTransport(r.envelope,[]{return true;}));}assert(svc.RetireTransportResume(r.envelope));};
  auto isolated=[&](const std::string& name){cfg.conversation.usage_dir=dir+"/"+name;};

  isolated("distinct-shared");
  {
    realtime::DailyUsage usage(cfg.conversation.usage_dir);
    auto a=std::make_unique<C>(cfg,callbacks,false,false,false,true);E ea=begin(*a);pcm(*a,ea);
    a->HandleTextBound(ea,delta("r1","q1"));a->HandleTextBound(ea,done("r1",10));assert(usage.ReadToday()==10);
    const int before=events;
    a->HandleTextBound(ea,delta("r2","q2"));assert(events==before);assert(a->UnsettledFreshUsage()==1);
    retire(*a,ea);
    C b(cfg,callbacks,false,false,false,true);successor(b,*a,true);assert(b.UnsettledFreshUsage()==1);a.reset();
    b.HandleTextBound(ea,done("r2",3));b.HandleTextBound(ea,done("r2",3));b.HandleTextBound(ea,done("r1",10));
    assert(usage.ReadToday()==13 && b.UnsettledFreshUsage()==0 && events==before);
    const auto s=b.InspectFreshUsage();assert(!s.storage_unknown && !s.attribution_unknown && !s.missing_totals);
    auto r2=std::find_if(s.receipts.begin(),s.receipts.end(),[](const auto& r){return r.response_id=="r2";});
    assert(r2!=s.receipts.end() && r2->origin==ea && r2->question_id=="q2" && r2->settled && r2->retired);
    assert(r2->source_type=="response.output_audio.delta" && r2->response_path=="$.response_id" && r2->question_path=="$.question_id" && r2->total_event_id=="bill-r2");
    // More provider responses can be observed only for billing after retirement.
    b.HandleTextBound(ea,delta("r3","q3"));b.HandleTextBound(ea,done("r3",4));b.HandleTextBound(ea,done("r3",4));
    assert(usage.ReadToday()==17 && b.UnsettledFreshUsage()==0 && events==before);
    std::cout<<"CASE distinct/shared/destroy-old ledger=17 pending=0 no_output_leak=1 provenance=1\n";
  }
  for(const auto& state:{std::string("session.closed"),std::string("error")}) {
    isolated(state);realtime::DailyUsage usage(cfg.conversation.usage_dir);
    C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);
    a.HandleTextBound(e,delta("r1","q1"));a.HandleTextBound(e,done("r1",10));
    a.HandleTextBound(e,J{{"type",state},{"error",{{"code","fixture"}}}}.dump());const int before=events;
    a.HandleTextBound(e,delta("r2","q2"));a.HandleTextBound(e,done("r2",3));a.HandleTextBound(e,done("r2",3));
    assert(usage.ReadToday()==13 && a.UnsettledFreshUsage()==0 && events==before);retire(a,e);
    C b(cfg,callbacks,false,false,false,true);successor(b,a,true);
    b.HandleTextBound(e,done("r2",3));assert(usage.ReadToday()==13);
    std::cout<<"CASE "<<state<<" second_known ledger=13 duplicate=0 output_leak=0\n";
  }
  isolated("missing-per-id");
  {
    realtime::DailyUsage usage(cfg.conversation.usage_dir);
    C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);
    a.HandleTextBound(e,delta("r1","q1"));a.HandleTextBound(e,done("r1",{}));
    a.HandleTextBound(e,delta("r2","q2"));a.HandleTextBound(e,done("r2",{}));
    assert(a.InspectFreshUsage().missing_totals==2 && usage.ReadToday()==0);retire(a,e);
    C b(cfg,callbacks,false,false,false,true);successor(b,a,false);
    b.HandleTextBound(e,done("r1",10));assert(b.InspectFreshUsage().missing_totals==1 && usage.ReadToday()==10);
    C c(cfg,callbacks,false,false,false,true);successor(c,b,false);
    c.HandleTextBound(e,done("r2",3));assert(c.InspectFreshUsage().missing_totals==0 && usage.ReadToday()==13);
    C d(cfg,callbacks,false,false,false,true);successor(d,c,true);
    std::cout<<"CASE missing-per-key two_marks -> one -> zero ledger=13 resume=1\n";
  }
  isolated("anonymous-first");
  {
    realtime::DailyUsage usage(cfg.conversation.usage_dir);
    C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);retire(a,e);
    assert(a.UnsettledFreshUsage()==1 && !a.InspectFreshUsage().missing_totals);
    C b(cfg,callbacks,false,false,false,true);successor(b,a,true);
    b.HandleTextBound(e,done("",{}));assert(b.InspectFreshUsage().missing_totals==1);
    b.HandleTextBound(e,done("",3));b.HandleTextBound(e,done("",3));assert(usage.ReadToday()==3 && b.UnsettledFreshUsage()==0);
    C c(cfg,callbacks,false,false,false,true);successor(c,b,true);
    std::cout<<"CASE no-done continues; anonymous-first missing recovers once ledger=3\n";
  }
  auto ambiguity=[&](const std::string& name,const auto& inject,uint64_t expected){
    isolated(name);realtime::DailyUsage usage(cfg.conversation.usage_dir);
    C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);inject(a,e);
    assert(usage.ReadToday()==expected && a.InspectFreshUsage().attribution_unknown);const int before=events;
    retire(a,e);C b(cfg,callbacks,false,false,false,true);successor(b,a,false);assert(events==before && b.InspectFreshUsage().attribution_unknown);
    std::cout<<"CASE "<<name<<" known_ledger="<<expected<<" attribution_UNKNOWN=1 successor_blocked=1\n";
  };
  ambiguity("anonymous-after-known",[&](C& a,E e){a.HandleTextBound(e,delta("r1","q1"));a.HandleTextBound(e,done("r1",10));a.HandleTextBound(e,done("",999));},10);
  ambiguity("anonymous-between-two",[&](C& a,E e){a.HandleTextBound(e,delta("r1","q1"));a.HandleTextBound(e,delta("r2","q2"));a.HandleTextBound(e,done("",3));},0);
  ambiguity("same-id-total-conflict",[&](C& a,E e){a.HandleTextBound(e,done("r1",10));a.HandleTextBound(e,done("r1",3));},10);
  ambiguity("same-id-question-conflict",[&](C& a,E e){a.HandleTextBound(e,done("r1",10,"q1"));a.HandleTextBound(e,done("r1",10,"q2"));},10);
  ambiguity("anonymous-question-conflict",[&](C& a,E e){a.HandleTextBound(e,done("",10,"q1"));a.HandleTextBound(e,done("",10,"q2"));},10);
  ambiguity("nested-id-conflict",[&](C& a,E e){a.HandleTextBound(e,R"({"type":"response.done","response_id":"r1","response":{"id":"r2"},"usage":{"total_tokens":3}})");},0);
  isolated("budget-preserved");
  {
    cfg.conversation.daily_token_limit=13;realtime::DailyUsage usage(cfg.conversation.usage_dir);
    C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);
    a.HandleTextBound(e,done("r1",10));a.HandleTextBound(e,done("r2",3));assert(usage.ReadToday()==13);retire(a,e);
    C b(cfg,callbacks,false,false,false,true);assert(b.InheritRetiredUsage(a));auto r=prepare(next_input++);assert(r.approved && b.PrepareFreshTransport(r.envelope));
    assert(b.Start());assert(!b.StartSession(2s,r.instructions));assert(usage.ReadToday()==13);retire(b,r.envelope);
    cfg.conversation.daily_token_limit=0;
    std::cout<<"CASE original daily budget13 survives new connection\n";
  }
  isolated("provider-receipt-cap");
  {
    realtime::DailyUsage usage(cfg.conversation.usage_dir);C a(cfg,callbacks,false,false,false,true);E e=begin(a);pcm(a,e);
    for(size_t i=0;i<128;++i)a.HandleTextBound(e,done("r"+std::to_string(i),1));
    assert(usage.ReadToday()==128 && a.InspectFreshUsage().receipts.size()==128);
    a.HandleTextBound(e,done("r128",1));auto s=a.InspectFreshUsage();assert(usage.ReadToday()==128 && s.attribution_unknown && s.receipts.size()==129);
    const auto& rejected=s.receipts.front();assert(rejected.conflict=="provider_response_observation_cap" && rejected.conflicting_response_id=="r128" && rejected.conflicting_total==1);
    retire(a,e);C b(cfg,callbacks,false,false,false,true);successor(b,a,false);
    std::cout<<"CASE 128 provider receipts cap explicit UNKNOWN rejected_r128_total=1 retained\n";
  }
  isolated("settled-retention");
  {
    realtime::DailyUsage usage(cfg.conversation.usage_dir);std::unique_ptr<C> old;E first;
    for(size_t i=0;i<129;++i) {
      auto c=std::make_unique<C>(cfg,callbacks,false,false,false,true);
      if(old)assert(c->InheritRetiredUsage(*old));old.reset();
      E e=begin(*c);if(i==0)first=e;pcm(*c,e);c->HandleTextBound(e,done("r"+std::to_string(i),1));retire(*c,e);
      assert(usage.ReadToday()==i+1 && c->UnsettledFreshUsage()==0 && !c->InspectFreshUsage().attribution_unknown);
      assert(c->InspectFreshUsage().receipts.size()<=128);old=std::move(c);
    }
    // Dedup beyond bounded retention is unprovable: block and never debit twice.
    old->HandleTextBound(first,done("r0",1));assert(usage.ReadToday()==129 && old->InspectFreshUsage().attribution_unknown);
    std::cout<<"CASE 129 settled transports continue bounded128; evicted late done UNKNOWN no_redebit\n";
  }
  svc.Stop();server.stop();std::filesystem::remove_all(dir);
}
