#ifdef NDEBUG
#error memory client tests require active assert
#endif
#include <cassert>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "memory/service.hpp"
#include "realtime/client.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
void CheckSentPrompts(realtime::Client& client, std::string& sent, std::mutex& sent_mu) {
 char dir[]="/tmp/xiaoai-coach-XXXXXX";assert(mkdtemp(dir));
 config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
 const memory::Json card={{"date","2026-10-23"},{"topic","Triceratops"},
   {"recap","画了恐龙，练习描述角和尾巴。"},{"resume_question","What plant would your dinosaur eat?"},
   {"new_words",memory::Json::array({"horn","tail","plant"})}};
 auto children=memory::Json::array();
 for(const auto& name:{"Amy","Ben"})children.push_back({{"id",name},{"version",1},
   {"profile",{{"name",std::string(name)+"Fixture"},{"aliases",memory::Json::array({name})},{"grade",std::string(name)=="Amy"?"二年级":"幼儿园大班"},
     {"level","simple sentences"},{"interests","drawing"},{"goals","ask questions"}}},
   {"summaries",memory::Json::array({card})}});
 std::mutex mu;std::vector<memory::Action> actions;
 memory::Service service(cfg,[&](const std::string& path,const memory::Json&)->memory::Reply {
   if(path=="/device/snapshot")return {200,{{"generation",0},{"expires_at",std::time(nullptr)+86400},{"children",children}}};
   return {503,memory::Json::object()};
 },[&](memory::Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(std::move(a));});
 auto wait=[&](const auto& condition){for(int i=0;i<500 && !condition();++i)std::this_thread::sleep_for(10ms);assert(condition());};
 auto action=[&](size_t n){wait([&]{std::lock_guard<std::mutex> l(mu);return actions.size()>n;});std::lock_guard<std::mutex> l(mu);return actions[n];};
 auto verify=[&](const char* label,const std::string& instructions){
   assert(client.StartSession(2s,instructions));std::string actual;
   {std::lock_guard<std::mutex> l(sent_mu);actual=sent;}
   assert(actual==instructions && actual.size()<=memory::kInstructionsBudget);
   (void)memory::Json(actual).dump(-1,' ',false,memory::Json::error_handler_t::strict);
   assert(actual.find("SAFETY FIRST")!=std::string::npos);
   assert(actual.find("VOLUME: if asked louder or softer (大声点/小声点), say only \"Okay!\". It is handled locally; never say you can't.")!=std::string::npos);
   assert(actual.find("Praise exactly what they did")!=std::string::npos);
   assert(actual.find("no grammar talk")!=std::string::npos);
   const std::string wait_rule="KIDS PAUSE to find words. If they clearly haven't finished (\"I want...\", one word, half a sentence), say only \"Mm-hm, go on.\" once per turn. Don't finish for them, don't answer early, don't change topic.";
   assert(actual.find(wait_rule)!=std::string::npos && actual.find(wait_rule)==actual.rfind(wait_rule));
   assert(actual.find("never say you can see them")!=std::string::npos);
   assert(actual.find("Usually 2 short sentences, never more than 3")==actual.rfind("Usually 2 short sentences, never more than 3"));
   for(const auto* rule:{"You are Doubao, an English playmate for Amy and Ben", "SAFETY FIRST", "VOLUME:", "Unless asked how to end", "never say the wake word 豆包豆包"})
     assert(actual.find(rule)!=std::string::npos && actual.find(rule)==actual.rfind(rule));
   for(const auto* game:{"1. Story:", "2. Riddles:", "3. Jokes:", "Simon says", "5. Silly Doubao:", "6. Guess the animal:"})
     assert(actual.find(game)!=std::string::npos);
   assert(actual.find("绝不输出第三句")==std::string::npos && actual.find("recast")==std::string::npos);
   std::cout<<label<<" prompt UTF-8 bytes="<<actual.size()<<std::endl;
   assert(client.FinishSession());return actual;
 };
 auto hear=[&](uint64_t epoch,const std::string& text){realtime::duplex::Event e;
   e.type="conversation.item.input_audio_transcription.completed";e.text=text;e.event_id=text;e.item_id=text;
   assert(service.Event(epoch,e));};
 service.Start();wait([&]{return service.cache_revision()>0;});
 assert(service.Begin(1));auto a=action(0);assert(a.identity_pending);
 auto actual=verify("ordinary/identity pending",a.instructions);
 assert(actual.find("Untrusted profile")==std::string::npos && actual.find("Triceratops")==std::string::npos);
 assert(service.Activate(1));hear(1,"I'm Amy");a=action(1);
 actual=verify("Amy identified",a.instructions);
 for(const auto& field:{"name=AmyFixture","grade=二年级","level=simple sentences","interests=drawing","goals=ask questions",
                       "2026-10-23","Triceratops","What plant would your dinosaur eat?","horn, tail, plant","画了恐龙，练习描述角和尾巴。"})
   assert(actual.find(field)!=std::string::npos);
 assert(actual.find("BenFixture")==std::string::npos);
 assert(service.Activate(1));hear(1,"We drew a blue dinosaur.");assert(service.End(1));assert(service.Begin(2));a=action(2);
 actual=verify("ten-minute resume",a.instructions);
 assert(!a.identity_pending && actual.find("This is a short resume")!=std::string::npos);
 assert(actual.find("The greeting already offered the last topic")==std::string::npos);
 assert(actual.find("We drew a blue dinosaur.")!=std::string::npos);
 assert(service.Activate(2));
 for(int i=0;i<8;++i)hear(2,"old-turn-"+std::to_string(i)+std::string(700,'x'));
 hear(2,"Newest topic: a blue kite.");assert(service.End(2));assert(service.Begin(3));a=action(3);
 actual=verify("UTF-8/recent-context bound",a.instructions);
 for(const auto& field:{"name=AmyFixture","grade=二年级","2026-10-23","Triceratops","What plant would your dinosaur eat?",
                       "horn, tail, plant","画了恐龙，练习描述角和尾巴。","Newest topic: a blue kite."})
   assert(actual.find(field)!=std::string::npos);
 assert(actual.find("old-turn-0")==std::string::npos);
 assert(service.Activate(3));hear(3,"I'm Ben");a=action(4);
 actual=verify("Ben identified",a.instructions);
 assert(actual.find("grade=幼儿园大班")!=std::string::npos && actual.find("AmyFixture")==std::string::npos);
 assert(service.Activate(3));hear(3,"I'm a guest");a=action(5);
 actual=verify("guest",a.instructions);
 for(const auto& field:{"AmyFixture","BenFixture","Untrusted profile","Triceratops","horn, tail, plant","二年级","幼儿园大班"})
   assert(actual.find(field)==std::string::npos);
 assert(service.Activate(3));hear(3,"I'm dad");a=action(6);
 actual=verify("adult",a.instructions);
 for(const auto& field:{"AmyFixture","BenFixture","Untrusted profile","Triceratops","horn, tail, plant","二年级","幼儿园大班"})
   assert(actual.find(field)==std::string::npos);
 service.Stop();std::filesystem::remove_all(dir);
}
int main(){
 const int port=ix::getFreePort();assert(port>0);ix::WebSocketServer server(port,"127.0.0.1");
 std::atomic<int> creates{0}, opens{0}, closes{0};std::atomic<bool> ack_close{true};std::string last_instructions;std::mutex mu;std::condition_variable cv;std::vector<realtime::duplex::Event> events;
 server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
  if(msg->type==ix::WebSocketMessageType::Open){++opens;return;}
  if(msg->type==ix::WebSocketMessageType::Close){++closes;return;}
  if(msg->type!=ix::WebSocketMessageType::Message)return;
  auto j=memory::Json::parse(msg->str);auto type=j.at("type");
  if(type=="session.create") {
   ++creates;{std::lock_guard<std::mutex> l(mu);last_instructions=j["session"]["instructions"];
    assert(last_instructions.find("Usually 2 short sentences, never more than 3")!=std::string::npos);
    assert(last_instructions.find("at most ONE question")!=std::string::npos);
    assert(last_instructions.size()<=memory::kInstructionsBudget);}
   assert(j["session"]["extension"]["dialog"]["extra"]["strict_audit"]==true);
   assert(j["session"]["extension"]["dialog"]["extra"]["enable_volc_websearch"]==false);assert(j["session"]["tools"].empty());
   ws.sendText(R"({"type":"session.created","session":{"id":"fresh-mock-session"}})");
  }else if(type=="speech_text_buffer.commit") {
   ws.sendText(R"({"type":"conversation.item.input_audio_transcription.delta","event_id":"delta1","item_id":"q1","delta":"I like"})");
   ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"asr1","item_id":"q1","content_index":0,"transcript":"I like dinosaurs."})");
   ws.sendText(R"({"type":"response.output_text.delta","event_id":"delta2","question_id":"q1","response_id":"r1","delta":"Which"})");
   ws.sendText(R"({"type":"response.output_text.done","event_id":"out1","question_id":"q1","response_id":"r1","text":"Which dinosaur?"})");
  }else if(type=="session.close" && ack_close) ws.sendText(R"({"type":"session.closed"})");
 });
 assert(server.listenAndStart());
 char dir[]="/tmp/xiaoai-memory-client-XXXXXX";assert(mkdtemp(dir));
 config::Config cfg;cfg.memory.enabled=true;cfg.realtime.api_key="fake-offline-key";
 cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);cfg.conversation.usage_dir=std::string(dir)+"/usage";
 realtime::Client::Callbacks callbacks;
 callbacks.on_text_event=[&](const auto& event){std::lock_guard<std::mutex> l(mu);events.push_back(event);cv.notify_all();};
 std::atomic<int> reply_events{0};
 callbacks.on_response_text=[&](const auto& e){assert(e.response_id=="r1");++reply_events;};
 realtime::Client client(cfg,callbacks);assert(client.Start());
 assert(client.StartSession(2s,memory::IdentityInstructions({"Amy","Ben"})));assert(client.SendSayHello("Is that Amy or Ben?"));
 {std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,2s,[&]{return events.size()==2;}));assert(events[0].text=="I like dinosaurs.");assert(events[0].item_id=="q1");assert(events[1].response_id=="r1");assert(events[1].question_id=="q1");assert(last_instructions.find("dinosaurs")==std::string::npos);assert(last_instructions.find(memory::IdentityInstructions({"Amy","Ben"}))==0);}
 assert(reply_events==2);
 assert(client.FinishSession());
 auto profile=memory::Json{{"profile",{{"name","BenFixture"}}},{"summaries",memory::Json::array({
   {{"date","2026-10-23"},{"session_id","fixture"},{"topic","Triceratops"},
    {"recap","Drew a dinosaur."},{"resume_question","What plant would your dinosaur eat?"},
    {"new_words",memory::Json::array({"horn","tail","plant"})}}
 })}};
 assert(client.StartSession(2s,memory::BuildInstructions(profile)));assert(creates==2);
 {std::lock_guard<std::mutex> l(mu);assert(last_instructions.find(memory::BuildInstructions(profile))==0);assert(last_instructions.find("name=BenFixture")!=std::string::npos);assert(last_instructions.find("AmyFixture")==std::string::npos);
   assert(last_instructions.find("Triceratops")!=std::string::npos);
   assert(last_instructions.find("What plant would your dinosaur eat?")!=std::string::npos);
   assert(last_instructions.find("horn")!=std::string::npos);}
 assert(client.FinishSession());
 ack_close=false;
 // Missing protocol ACK must not delay replacement or leave live sockets.
 for(int n=0;n<12;++n) {
   assert(client.StartSession(2s,memory::IdentityInstructions({"Amy","Ben"})));
   const auto start=std::chrono::steady_clock::now();
   assert(client.FinishSession(0ms));
   assert(client.StartSession(2s,memory::BuildInstructions(profile)));
   assert(std::chrono::steady_clock::now()-start<1s);
   assert(client.FinishSession(0ms));
 }
 ack_close=true;CheckSentPrompts(client,last_instructions,mu);
 client.Stop();server.stop();assert(opens==closes);assert(opens==33);std::filesystem::remove_all(dir);
}
