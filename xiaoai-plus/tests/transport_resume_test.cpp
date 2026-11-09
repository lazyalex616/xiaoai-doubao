#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <iostream>
#include <thread>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "memory/service.hpp"
#include "realtime/client.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using memory::Json;
using E=realtime::TransportEnvelope;
using C=realtime::Client;
using R=memory::ResumeResult;
template<class F> R Reply(F f) {
  std::promise<R> promise;auto future=promise.get_future();
  assert(f([&](R r){promise.set_value(std::move(r));}));
  assert(future.wait_for(3s)==std::future_status::ready);return future.get();
}
template<class F> void Wait(F f) {for(int i=0;i<300 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());}
realtime::duplex::Event ASR(std::string text,std::string item="fixture-item") {
  realtime::duplex::Session s;s.Create("fixture","fixture");s.Receive(R"({"type":"session.created","session":{"id":"fixture"}})");
  return s.Receive(Json{{"type","conversation.item.input_audio_transcription.completed"},{"event_id",item+"-event"},
                        {"item_id",item},{"transcript",text}}.dump());
}
Json Active(const std::string& dir) {std::ifstream in(dir+"/active.json");Json j;in>>j;return j;}
int main() {
  char temp[]="/tmp/transport-resume-XXXXXX";assert(mkdtemp(temp));const std::string dir=temp;
  config::Memory mc;mc.enabled=true;mc.cache_dir=dir+"/memory";
  std::atomic<int64_t> version{4},expiry{std::time(nullptr)+3600},generation{9};
  std::atomic<int> actions{0};
  memory::Service svc(mc,[&](const std::string& path,const Json&)->memory::Reply {
    if(path=="/device/snapshot")return {200,{{"generation",generation.load()},{"expires_at",expiry.load()},
      {"children",Json::array({Json{{"id","amy"},{"version",version.load()},
        {"profile",{{"name","Amy"},{"aliases",Json::array({"Amy"})},{"grade","2"},{"secret","fixture-secret-must-not-export"}}},
        {"summaries",Json::array({Json{{"recap","fixture learning recap"}}})}},
       Json{{"id","ben"},{"version",4},{"profile",{{"name","Ben"},{"aliases",Json::array({"Ben"})}}},{"summaries",Json::array()}}})}}};
    return {200,Json::object()};
  },[&](memory::Action){++actions;});
  svc.Start();Wait([&]{return svc.cache_revision()>0;});assert(svc.Begin(42));Wait([&]{return actions==1;});
  assert(svc.Event(42,ASR("I am Amy")));Wait([&]{return actions==2;});assert(svc.Activate(42));
  auto inspect=[&](uint64_t epoch=42){return Reply([&](auto cb){return svc.InspectTransportResume(epoch,cb);});};
  const auto base=inspect();assert(base.approved && base.identity.kind=="child" && base.identity.child_id=="amy");
  assert(base.instructions.find("fixture learning recap")!=std::string::npos);
  assert(base.instructions.find("fixture-secret")==std::string::npos);
  const auto active_id=Active(mc.cache_dir).at("session_id");
  assert(!inspect(41).approved);
  auto prepare=[&](memory::ResumeIdentity stamp,uint64_t t,uint64_t capture,uint64_t input) {
    return Reply([&](auto cb){return svc.PrepareTransportResume(stamp,t,capture,input,cb);});
  };
  auto wrong=base.identity;wrong.child_id="ben";assert(!prepare(wrong,1,77,1).approved);
  wrong=base.identity;wrong.profile_version=3;assert(!prepare(wrong,1,77,1).approved);
  wrong=base.identity;wrong.generation=8;assert(!prepare(wrong,1,77,1).approved);
  auto first=prepare(base.identity,1,77,1);assert(first.approved);const E old=first.envelope;
  assert(!prepare(base.identity,1,77,1).approved);
  assert(svc.Event(42,ASR("I am Ben"))); // Retired unbound callback, same epoch.
  assert(svc.End(42));assert(svc.Interrupt(42));assert(svc.Activate(42));assert(svc.Begin(42));
  assert(svc.Event(old,ASR("fixture pregrant speech")));
  assert(inspect().identity.child_id=="amy" && Active(mc.cache_dir).at("session_id")==active_id);
  assert(Active(mc.cache_dir).at("turns").empty());

  const int port=ix::getFreePort();assert(port>0);ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> creates{0},appends{0};std::mutex pacing_mu;std::vector<std::chrono::steady_clock::time_point> paced;
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type!=ix::WebSocketMessageType::Message)return;const auto j=Json::parse(msg->str);
    if(j.at("type")=="session.create")ws.sendText(Json{{"type","session.created"},{"session",{{"id","fixture-session-"+std::to_string(++creates)}}}}.dump());
    else if(j.at("type")=="input_audio_buffer.append"){
      std::lock_guard<std::mutex> lock(pacing_mu);paced.push_back(std::chrono::steady_clock::now());++appends;
    }
  });assert(server.listenAndStart());
  config::Config cfg;cfg.realtime.api_key="synthetic-loopback-only";
  cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);cfg.conversation.usage_dir=dir+"/usage";
  std::atomic<int> events{0},untagged{0};C::Callbacks callbacks;
  callbacks.on_asr_final=[&](const auto&){++untagged;};callbacks.on_text_event=[&](const auto&){++untagged;};
  callbacks.on_audio=[&](const auto&){++untagged;};callbacks.on_chat_ended=[&]{++untagged;};
  callbacks.on_exit_intent=[&]{++untagged;};callbacks.on_session_closed=[&](const auto&){++untagged;};
  callbacks.on_transport_event=[&](const E& e,const auto& event){++events;assert(svc.Event(e,event));};
  C client(cfg,callbacks,false,false,false,true);assert(client.PrepareFreshTransport(old));assert(client.Start());assert(client.StartSession(2s,first.instructions));
  assert(!client.SendSayHello());assert(!client.EnqueueAudio(std::vector<uint8_t>(640,0)));
  const std::string asr=Json{{"type","conversation.item.input_audio_transcription.completed"},{"event_id","asr-1"},{"item_id","input-1"},{"transcript","fixture new question"}}.dump();
  client.HandleTextBound(old,asr);assert(events==1); // Only real loopback session.created.
  auto activate=[&](E e){return Reply([&](auto cb){return svc.ActivateTransportResume(e,cb);});};
  const auto permit=activate(old);assert(permit.approved && permit.permit.valid());assert(!activate(old).approved);
  realtime::FreshTransportOwner::Proof proof;
  assert(!client.GrantFreshInput(permit.permit,"fixture-session-1",77,proof));
  proof={true,true,true,true,true,false};
  assert(!client.GrantFreshInput({},"fixture-session-1",77,proof));
  assert(!client.GrantFreshInput(permit.permit,"wrong-session",77,proof));
  assert(!client.GrantFreshInput(permit.permit,"fixture-session-1",78,proof));
  auto native=proof;native.native_blocked=true;assert(!client.GrantFreshInput(permit.permit,"fixture-session-1",77,native));
  assert(client.GrantFreshInput(permit.permit,"fixture-session-1",77,proof));assert(!client.GrantFreshInput(permit.permit,"fixture-session-1",77,proof));
  assert(client.EnqueueFreshAudio(old,std::vector<uint8_t>(1920,0)));Wait([&]{return appends==3;});
  {std::lock_guard<std::mutex> lock(pacing_mu);assert(paced.size()==3);for(size_t i=1;i<3;++i)assert(paced[i]-paced[i-1]>=15ms);}
  client.HandleTextBound(old,asr);
  client.HandleTextBound(old,R"({"type":"response.output_text.done","event_id":"text-1","question_id":"different-provider-question","response_id":"response-1","text":"fixture answer"})");
  client.HandleTextBound(old,R"({"type":"response.output_audio.delta","event_id":"audio-1","question_id":"different-provider-question","response_id":"response-1","delta":"AQI="})");
  client.HandleTextBound(old,R"({"type":"response.done","response_id":"response-1","usage":{"total_tokens":10}})");
  auto same=inspect();assert(same.approved && same.identity==base.identity && actions==2);
  auto saved=Active(mc.cache_dir);assert(saved.at("session_id")==active_id && saved["turns"].size()==2);
  assert(saved["turns"][1]["incomplete"]==true && saved["complete"]==false);
  realtime::DailyUsage usage(cfg.conversation.usage_dir);assert(usage.ReadToday()==10);
  const int before=events;
  assert(client.RetireFreshTransport(old,[&]{
    std::thread reentry([&]{client.HandleTextBound(old,asr);assert(!client.PrepareFreshTransport(old));});reentry.join();return true;
  }));assert(svc.RetireTransportResume(old));
  auto second=prepare(inspect().identity,2,77,2);assert(second.approved);E next=second.envelope;
  assert(!permit.permit.valid());
  assert(client.PrepareFreshTransport(next));assert(client.StartSession(2s,second.instructions));
  const int after_created=events;
  for(const auto& wire:{asr,std::string(R"({"type":"response.output_audio.delta","delta":"AwQ="})"),
      std::string(R"({"type":"response.output_text.done","event_id":"late","response_id":"response-1","question_id":"different-provider-question","text":"fixture stale"})"),
      std::string(R"({"type":"response.output_audio.done","status_code":20000002})"),
      std::string(R"({"type":"response.done","response_id":"response-1","usage":{"total_tokens":10}})")})client.HandleTextBound(old,wire);
  assert(events==after_created && after_created==before+1 && usage.ReadToday()==10 && untagged==0);
  assert(svc.Event(old,ASR("I am Ben")));assert(inspect().identity.child_id=="amy");
  assert(svc.End(old));assert(svc.Interrupt(old));assert(inspect().approved);
  client.HandleTextBound(next,asr);assert(events==after_created); // fresh ID/ASR never grant.
  auto p2=activate(next);assert(p2.approved);assert(!client.GrantFreshInput(permit.permit,"fixture-session-2",77,proof));
  assert(client.GrantFreshInput(p2.permit,"fixture-session-2",77,proof));
  const int bound=events;
  for(const auto& wire:{R"({"type":"response.output_audio.delta","delta":"AQI="})",
    R"({"type":"response.output_text.done","event_id":"invalid","response_id":12,"question_id":"q","text":"fixture invalid"})",
    R"({"type":"response.output_text.done","event_id":"conflict","response_id":"r","response":{"id":"different"},"question_id":"q","text":"fixture conflict"})"})client.HandleTextBound(next,wire);
  assert(events==bound);
  client.HandleTextBound(next,R"({"type":"response.output_text.delta","event_id":"delta-2","response_id":"r2","question_id":"q2","delta":"fixture"})");
  client.HandleTextBound(next,R"({"type":"response.done","response_id":"r2","usage":{"total_tokens":5}})");
  assert(usage.ReadToday()==15);
  assert(client.RetireFreshTransport(next,[]{return true;}));assert(svc.RetireTransportResume(next));
  auto third=prepare(inspect().identity,3,77,3);assert(third.approved);
  assert(client.PrepareFreshTransport(third.envelope));assert(client.StartSession(2s,third.instructions));auto p3=activate(third.envelope);
  assert(client.GrantFreshInput(p3.permit,"fixture-session-3",77,proof));
  client.HandleTextBound(third.envelope,R"({"type":"response.output_text.delta","event_id":"delta-3","response_id":"r3","question_id":"q3","delta":"fixture"})");
  const int accepted=events;
  client.HandleTextBound(third.envelope,R"({"type":"response.output_text.delta","event_id":"conflicting-turn","response_id":"r-other","question_id":"q3","delta":"fixture"})");
  client.HandleTextBound(third.envelope,asr);assert(events==accepted); // Conflicting IDs retire owner.
  assert(client.RetireFreshTransport(third.envelope,[]{return true;}));assert(svc.RetireTransportResume(third.envelope));
  assert(client.UnsettledFreshUsage()==2); // Two observed provider response keys, even rejected output has separate billing.
  const int retired_events=events;
  client.HandleTextBound(third.envelope,R"({"type":"response.done","response_id":"r3","usage":{"total_tokens":3}})");
  client.HandleTextBound(third.envelope,R"({"type":"response.done","response_id":"r3","usage":{"total_tokens":3}})");
  client.HandleTextBound(third.envelope,R"({"type":"response.done","response_id":"r-other","usage":{"total_tokens":0}})");
  assert(client.UnsettledFreshUsage()==0 && usage.ReadToday()==18 && events==retired_events);
  auto fourth=prepare(inspect().identity,4,77,4);assert(fourth.approved);
  assert(client.PrepareFreshTransport(fourth.envelope));assert(client.StartSession(2s,fourth.instructions));
  auto p4=activate(fourth.envelope);assert(p4.approved && client.GrantFreshInput(p4.permit,"fixture-session-4",77,proof));
  client.HandleTextBound(fourth.envelope,R"({"type":"response.done","usage":{}})");
  assert(client.RetireFreshTransport(fourth.envelope,[]{return true;}));
  assert(!client.PrepareFreshTransport(E(42,1,9,4,100,100,77,100))); // Existing explicit missing-total rule.
  // An anonymous extra total after known r1 is ambiguous: no invented debit or next grant.
  client.HandleTextBound(old,R"({"type":"response.done","usage":{"total_tokens":999}})");
  const auto ambiguous=client.InspectFreshUsage();assert(ambiguous.attribution_unknown && ambiguous.missing_totals==1);
  assert(!client.PrepareFreshTransport(E(42,1,9,4,101,101,77,101)));
  assert(usage.ReadToday()==18);client.Stop();
  C inherited(cfg,{},false,false,false,true);assert(inherited.InheritRetiredUsage(client));
  assert(!inherited.PrepareFreshTransport(E(42,1,9,4,100,100,77,100))); // Existing UNKNOWN survives object replacement.
  config::Config limited=cfg;limited.conversation.daily_token_limit=18;
  C budget(limited,{},false,false,false,true);assert(budget.PrepareFreshTransport(fourth.envelope));assert(budget.Start());assert(!budget.StartSession(50ms));budget.Stop();
  assert(svc.RetireTransportResume(fourth.envelope));
  auto stale=inspect().identity;version=5;auto rev=svc.cache_revision();assert(svc.Prefetch());Wait([&]{return svc.cache_revision()>rev;});
  assert(!prepare(stale,5,77,5).approved && inspect().identity.profile_version==5); // Next input adopts latest profile facts.
  assert(svc.Begin(43));Wait([&]{return actions==3;});assert(svc.Event(43,ASR("I am dad")));Wait([&]{return actions==4;});assert(svc.Activate(43));
  auto adult=inspect(43);assert(adult.approved && adult.identity.kind=="adult" && adult.identity.child_id.empty());
  assert(adult.instructions.find("fixture learning recap")==std::string::npos);
  auto a=prepare(adult.identity,5,88,5);assert(a.approved);auto ap=activate(a.envelope);assert(ap.approved);
  realtime::FreshTransportOwner live_owner(true);assert(live_owner.Prepare(a.envelope));assert(live_owner.SessionCreated(a.envelope,"fixture-policy-session"));
  assert(live_owner.Grant(ap.permit,"fixture-policy-session",88,proof));assert(live_owner.Input(a.envelope));
  assert(svc.Event(a.envelope,ASR("I am Ben","adult-switch")));auto changed=inspect(43);assert(!changed.approved);
  assert(!ap.permit.valid() && !live_owner.Input(a.envelope)); // Shared revoke before App action delivery.
  assert(!activate(a.envelope).approved);assert(svc.Activate(43));auto ben=inspect(43);assert(ben.approved && ben.identity.child_id=="ben");
  auto n=prepare(ben.identity,6,88,6);assert(n.approved);assert(activate(n.envelope).approved);
  assert(svc.Event(n.envelope,ASR("再见","fixture-exit")));assert(!inspect(43).approved && !activate(n.envelope).approved);
  assert(svc.Begin(44));Wait([&]{return actions==7;});assert(svc.Event(44,ASR("I am dad")));Wait([&]{return actions==8;});assert(svc.Activate(44));
  const auto before_expiry=inspect(44);assert(before_expiry.approved);
  expiry=std::time(nullptr)-1;rev=svc.snapshot_receipts();assert(svc.Prefetch());Wait([&]{return svc.snapshot_receipts()>rev;});
  assert(!inspect(44).approved && !prepare(before_expiry.identity,7,88,7).approved);
  svc.Stop();server.stop();std::filesystem::remove_all(dir);
  std::cout<<"PASS real_Service_thread same_child_resume_preserves_identity_active_session_snapshot no_secret_export\n"
           <<"PASS stale_epoch_child_profile_snapshot rejected owner_grant_requires_Service_permit_session_capture_proof\n"
           <<"PASS registered_WS_envelope late_audio_text_ASR_done_usage_rejected no_untagged_callbacks no_implicit_grant\n"
           <<"PASS missing_invalid_conflicting_IDs refused correlation_unknown_saved_incomplete budget=18_retained old_usage_settles_once existing_unknown_blocks\n"
           <<"PASS adult_no_child_history identity_switch_exit_expiry_revoke serialized_leases local_contract_fixture_only\n"
           <<"PASS fresh_input_real_sender_20ms_pacing three_synthetic_loopback_frames no_provider_audio\n"
           <<"PASS quiescent_successor_inherits_main_voice_unknown same_ledger_policy_required\n";
}
