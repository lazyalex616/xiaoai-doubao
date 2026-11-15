#ifdef NDEBUG
#error memory tests require active assert
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "memory/service.hpp"
using namespace xiaoai_plus;
using memory::Json;
Json Children() {
 return Json::array({Json{{"id","amy"},{"version",1},{"profile",{{"name","AmyFixture"},{"aliases",Json::array({"Amy","同学甲"})}}},{"summaries",Json::array()}},
                     Json{{"id","ben"},{"version",1},{"profile",{{"name","BenFixture"},{"aliases",Json::array({"Ben","同学乙"})}}},{"summaries",Json::array()}}});
}
realtime::duplex::Event Text(std::string type,std::string text,std::string event,std::string item="",std::string response="",std::string question="") {
 realtime::duplex::Event e;e.type=type;e.text=text;e.event_id=event;e.item_id=item;e.response_id=response;e.question_id=question;return e;
}
constexpr auto user="conversation.item.input_audio_transcription.completed";
constexpr auto answer="response.output_text.done";
void Wait(const std::function<bool()>& f) {for(int i=0;i<500 && !f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(f());}
memory::Decision GuestAfterRetry(memory::Identity& identity,const std::string& text,memory::Candidate candidate={}) {
  const auto retry=identity.Hear(text,candidate);
  assert(!retry.changed && retry.kind=="unknown" && retry.greeting==identity.question());
  return identity.Hear(text,candidate);
}
int main() {
 assert(memory::IdentityQuestion({})=="Hi! What's your name? Or are you a grown-up?");
 assert(memory::IdentityQuestion({"Amy"})=="Hi! Are you Amy? Or a grown-up?");
 assert(memory::IdentityQuestion({"Amy","Ben","Cat"})=="Hi! Are you Amy, Ben or Cat? Or a grown-up?");
 for(const auto& names:std::vector<std::vector<std::string>>{{},{"Amy"},{"Amy","Ben","Cat"}}) {
   const auto seeds=memory::SeedNames(names);assert(seeds.size()==names.size());
   for(size_t i=0;i<names.size();++i)assert(seeds[i]["profile"]["name"]==names[i] && seeds[i]["profile"]["aliases"]==memory::Json::array({names[i]}));
   const auto instructions=memory::IdentityInstructions(names);
   const std::string audience=names.empty()?"for Chinese kids learning English":names.size()==1?"for Amy, a Chinese kid learning English":"for Amy, Ben and Cat, Chinese kids learning English";
   const std::string call=names.empty()?"Call them by name and use":names.size()==1?"Call them by name (Amy) and use":"Call them by name (Amy, Ben or Cat) and use";
   assert(instructions.find(audience)!=std::string::npos && instructions.find(call)!=std::string::npos);
 }

 realtime::duplex::Session protocol;
 auto create=Json::parse(protocol.Create("test","voice",true));
 assert(create["session"]["extension"]["dialog"]["extra"]["strict_audit"]==true);
 assert(create["session"]["extension"]["dialog"]["extra"]["enable_volc_websearch"]==false);
 assert(create["session"]["tools"].empty());
 protocol.Receive(R"({"type":"session.created","session":{"id":"s"}})");
 auto asr=protocol.Receive(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"e1","item_id":"q1","content_index":0,"transcript":"I like dinosaurs."})");
 auto final=protocol.Receive(R"({"type":"response.output_text.done","event_id":"e2","question_id":"q1","response_id":"r1","text":"Which dinosaur?"})");
 assert(asr.item_id=="q1" && final.question_id=="q1" && final.response_id=="r1");
 memory::Transcript transcript;
 auto delta=protocol.Receive(R"({"type":"response.output_text.delta","event_id":"ed","response_id":"r1","delta":"Which"})");
 assert(!transcript.Add(delta));assert(transcript.Add(asr));assert(!transcript.Add(asr));
 asr.event_id="reissued";assert(!transcript.Add(asr));assert(transcript.Add(final));
 transcript.Interrupted("r1");assert(transcript.turns()[1]["interrupted"]==true);
 memory::Transcript missing;assert(missing.Add(Text(answer,"hello","")));assert(!missing.complete());
 assert(memory::IdentityLogText(std::string(30,'x'))==std::string(24,'x'));
 std::string unicode;for(int i=0;i<30;++i)unicode+="艾";
 assert(memory::IdentityLogText(unicode).size()==24*3);
 assert(memory::IdentityLogText("Amy\n\tBen")=="AmyBen");
 // Initial and retry prompts share normalization and longest suffix removal.
 for(const char* question:{"Hi! Are you Amy or Ben? Or a grown-up?","Is that Amy or Ben? Or a grown-up?"}) {
  memory::Identity echoed(question);
  for(const auto& sample:std::vector<std::pair<std::string,std::string>>{
      {"Or a grown up 我是Amy","amy"},{"Or a grown up, Ben","ben"},
      {"grown up 我是爸爸","adult"},{"A grown-up","adult"},{"I'm a grown-up","adult"},
      {"成年人","adult"},{"Ben","ben"},{"Amy","amy"}}) {
   echoed.Reset(Children());assert(echoed.Rule(sample.first)==sample.second);
   const auto d=echoed.Hear(sample.first);
   assert(d.changed && (sample.second=="adult"?d.kind=="adult":d.child_id==sample.second));
  }
  for(const char* text:{"Or a grown up 关闭","不是Amy","Or a grown up 不是Amy"}) {
   echoed.Reset(Children());assert(echoed.Rule(text).empty() || echoed.Rule(text)=="guest");
   assert(GuestAfterRetry(echoed,text).kind=="guest");assert(echoed.child_id().empty());
  }
 }
 memory::Identity old_prompt("Is that Amy or Ben?");old_prompt.Reset(Children());
 assert(old_prompt.Hear("Amy or Ben 我是Amy").child_id=="amy");
 memory::Identity unicode_prompt("请告诉我你叫什么名字");unicode_prompt.Reset(Children());
 assert(unicode_prompt.Hear("诉我你叫什么名字 我是Ben").child_id=="ben");
 unicode_prompt.Reset(Children());assert(unicode_prompt.Hear("叫什么名字 我是Ben").child_id=="ben");
 memory::Identity tail_probe(memory::IdentityQuestion({"Amy","Ben"}));
 for(const char* text:{"up","Or a grown up i am amy","Or a grown up going up."})assert(!tail_probe.IsQuestionTail(text));
 tail_probe.Reset(Children());assert(tail_probe.Hear("Or a grown up i am amy").child_id=="amy");
 tail_probe.Reset(Children());assert(!tail_probe.Hear("Or a grown up going up.").greeting.empty());
 memory::Identity identity(memory::IdentityQuestion({"Amy","Ben"}));identity.Reset(Children());assert(identity.Hear("Amy").child_id=="amy");
 assert(!identity.Hear("A story about Ben").changed);assert(identity.Hear("No, I'm Ben").child_id=="ben");
 assert(identity.Hear("我是爸爸").kind=="adult");
 identity.Reset(Children());assert(identity.Hear("I'm daddy").kind=="adult");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"unclear").kind=="guest");assert(!identity.Hear("unclear again").changed);
 identity.Reset(Children());assert(identity.Hear("同学甲").child_id=="amy");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"story",{"amy",0.99}).kind=="guest");
 for(const char* t:{"爸爸","是爸爸。","我是爸爸！","妈妈","大人","I'm dad","I’m daddy!","I AM MOM","I'm mommy","我是他爹","我是他爸","我是家长","我是她妈妈","I'm their dad","I am their mom","I'm a parent","I'm an adult"}) {identity.Reset(Children());assert(identity.Hear(t).kind=="adult");}
 for(const char* t:{"AMY!!!","Hi, I'm Amy.","My name is Amy!","我是Amy呀","Amy!"}) {identity.Reset(Children());assert(identity.Hear(t).child_id=="amy");}
 identity.Reset(Children());assert(identity.Hear("Ben").child_id=="ben");assert(identity.Hear("No, I am Amy!").child_id=="amy");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"Amy and Ben").kind=="guest");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"I am not Amy").kind=="guest");
 auto ambiguous=Children();ambiguous[1]["profile"]["aliases"].push_back("Amy");identity.Reset(ambiguous);assert(GuestAfterRetry(identity,"Amy").kind=="guest");
 for(const char* t:{"Emmy","Aimee","Emmy","艾米","Amie","Nola","Nuola","Nic","Nik","Nroa","不是 Amy","I am not Amy","Amy 说我是 Ben","我是 Ben 的同学","\"Amy\"","'Amy'","‘Amy’","＂Ａｍｙ＂","＇Ａｍｙ＇","「Amy」"}) {
   identity.Reset(Children());assert(GuestAfterRetry(identity,t,{"amy",1.0}).kind=="guest");
 }
 for(const char* t:{"我是 Emmy","我是 Aimee","我是艾米","我是 Ben 的同学","不是 Amy"}) {
   identity.Reset(Children());assert(identity.Hear("Amy").child_id=="amy");
   if(std::string(t)=="不是 Amy")assert(identity.Hear(t).kind=="guest" && identity.child_id().empty());
   else {assert(!identity.Hear(t).changed);assert(identity.child_id()=="amy");}
 }
 // Identity answers may contain extra words; established identities survive ordinary chat.
 for(const char* text:{"I am ben today want you","Or a grown up 继续说， Ben，","Ben, I want a story","我是Ben今天想听故事"}) {
   identity.Reset(Children());assert(identity.Rule(text)=="ben");
   assert(identity.Hear(text).child_id=="ben");
 }
 for(const char* text:{"Ben's sister","Ben’s toy","Amy or Ben","Ben or a grown up","Benjamin","Amie","I am not Ben"}) {
   identity.Reset(Children());assert(identity.Rule(text)!="ben" && identity.Rule(text)!="amy");
   assert(GuestAfterRetry(identity,text).kind=="guest");
 }
 identity.Reset(Children());assert(identity.Hear("Ben").child_id=="ben");
 for(const char* text:{"It's a cat.","I'm a tiger.","是的","是","不是","Not yet.","I'm not sure.","我是老虎"}) {
   assert(!identity.Hear(text).changed);
   assert(identity.kind()=="child" && identity.child_id()=="ben");
 }
 assert(identity.Rule("I'm not Ben")=="guest");assert(identity.Hear("I'm not Ben").kind=="guest");
 identity.Reset(Children());identity.Hear("Ben");
 assert(identity.Rule("不是，我是Amy")=="amy");assert(identity.Hear("不是，我是Amy").child_id=="amy");
 identity.Reset(Children());identity.Hear("Ben");
 assert(identity.Rule("我是爸爸")=="adult");assert(identity.Hear("我是爸爸").kind=="adult");
 identity.Reset(Children());identity.Hear("Amy");assert(!identity.Hear("Amy 说她喜欢 Ben").changed);
 assert(!identity.Hear("Ben 来了").changed);assert(identity.Hear("我是奶奶").kind=="adult");
 auto registered=Children();registered[0]["profile"]["aliases"].push_back("小艾");
 identity.Reset(registered);assert(identity.Hear("我是小艾").child_id=="amy");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"opaque",{"ben",1.0}).kind=="guest");
 identity.Reset(Children());assert(GuestAfterRetry(identity,"opaque",{"evil",1.0}).kind=="guest");
 for(const char* text:{"艾米","艾咪","本恩","本本"}) {
   identity.Reset(Children());assert(GuestAfterRetry(identity,text).kind=="guest"); // No built-in phonetic names.
 }
 auto aliases=Children();aliases[0]["profile"]["aliases"].push_back("艾咪");aliases[1]["profile"]["aliases"].push_back("本本");
 identity.Reset(aliases);assert(identity.Hear("我是艾咪").child_id=="amy");
 identity.Reset(aliases);assert(identity.Hear("我是本本").child_id=="ben");
 aliases[1]["profile"]["aliases"].push_back("艾咪");aliases[0]["profile"]["aliases"].push_back("艾");identity.Reset(aliases);
 assert(GuestAfterRetry(identity,"艾咪").kind=="guest"); // Duplicated profile aliases cannot grant either child.
 for(const char* text:{"A m y！","Ａｍｙ！","我是　ＡＭＹ。","Ａｍｙ【】—…"}) {identity.Reset(Children());assert(identity.Hear(text).child_id=="amy");}
 for(const char* text:{"B e n。","ＢｅＮ，","我是　ＢＥＮ！"}) {identity.Reset(Children());assert(identity.Hear(text).child_id=="ben");}
 auto other=Children();other[0]["profile"]["name"]="Alex";other[0]["profile"]["aliases"]=Json::array({"小明呀"});
 identity.Reset(other);assert(identity.Hear("我是小明呀").child_id=="amy");
 identity.Reset(other);assert(identity.Hear("Ａ ｌ ｅ ｘ！").child_id=="amy");
 other[1]["profile"]["aliases"].push_back("小々");identity.Reset(other);assert(identity.Hear("小々").child_id=="ben");
 identity.Reset(Children());assert(identity.Hear("unclear").kind=="unknown");
 assert(identity.Hear("Ben").child_id=="ben");
 identity.Reset(Children());assert(identity.Hear("unclear").kind=="unknown"); // New session resets the attempt count.
 assert(identity.Hear("unclear").kind=="guest");
 auto child=Children()[0];child["profile"]["interests"]=std::string(10000,'x');child["summaries"]=Json::array();
 for(int i=0;i<8;i++)child["summaries"].push_back({{"recap",std::string(5000,'y')},{"resume_question",std::string(2000,'z')}});
 auto instructions=memory::BuildInstructions(child);assert(instructions.size()<=memory::kInstructionsBudget);assert(instructions.find("SAFETY FIRST")!=std::string::npos);
 assert(memory::Redact("email me at a@b.com or 13812345678").find("a@b.com")==std::string::npos);
 assert(memory::Redact("我住在北京路123号。").find("北京路")==std::string::npos);
 config::Config off;assert(!off.memory.enabled);realtime::duplex::Session legacy;auto original=Json::parse(legacy.Create("old","voice"));assert(!original["session"].contains("extension"));
 char dir[]="/tmp/xiaoai-memory-test-XXXXXX";assert(mkdtemp(dir));
 config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
 std::mutex mu;std::vector<memory::Action> actions;std::vector<Json> uploads;
 std::atomic<int> calls{0}, gen{0}, identity_calls{0};std::atomic<bool> unreachable{false}, slow{false};
 memory::Service service(cfg,[&](const std::string& path,const Json& body)->memory::Reply {
  ++calls;if(path=="/device/identity")++identity_calls;
  if(slow)std::this_thread::sleep_for(std::chrono::milliseconds(200));
  if(unreachable)return {};
  if(path=="/device/snapshot")return {200,{{"generation",gen.load()},{"expires_at",std::time(nullptr)+86400},{"children",Children()}}};
  if(path=="/device/sessions"){std::lock_guard<std::mutex> l(mu);uploads.push_back(body);return {200,{{"ack",body["session_id"]}}};}
  return {200,{{"ok",true}}};
 },[&](memory::Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(a);});
 service.Start();Wait([&]{return service.cache_revision()>0;});assert(service.Begin(1));
 auto count=[&]{std::lock_guard<std::mutex> l(mu);return actions.size();};Wait([&]{return count()>=1;});
 assert(service.Activate(1));
 for(const char* tail:{"Or a grown up.","Or grown up.","Ben? Or a grown-up?"}) {
   memory::Identity probe(memory::IdentityQuestion({"Amy","Ben"}));assert(probe.IsQuestionTail(tail));
   assert(service.Event(1,Text(user,tail,tail)));
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
   assert(count()==1 && identity_calls.load()==0);
 }
 assert(service.Event(1,Text(user,"I am Amy.","identity")));Wait([&]{return count()>=2;});
 assert(service.Activate(1));assert(service.Event(1,asr));assert(service.Event(1,final));assert(service.End(1));
 Wait([&]{std::lock_guard<std::mutex> l(mu);return uploads.size()==1;});
 {std::lock_guard<std::mutex> l(mu);assert(uploads[0]["child_id"]=="amy");assert(uploads[0]["turns"].size()==2);}
 assert(service.Begin(2));Wait([&]{return count()>=3;});assert(service.Activate(2));assert(service.Event(2,Text(user,"I'm daddy","adult")));Wait([&]{return count()>=4;});assert(service.Activate(2));assert(service.Event(2,asr));assert(service.End(2));
 std::this_thread::sleep_for(std::chrono::milliseconds(100));{std::lock_guard<std::mutex> l(mu);assert(uploads.size()==1);}
 unreachable=true;assert(service.Begin(3));Wait([&]{return count()>=5;});assert(service.Activate(3));assert(service.Event(3,Text(user,"I am Ben","ben")));Wait([&]{return count()>=6;});
 {std::lock_guard<std::mutex> l(mu);assert(actions.back().greeting.find("Hi, BenFixture!")==0 && actions.back().after_reply);assert(actions.back().instructions.find("AmyFixture")==std::string::npos);}
 assert(service.Activate(3));assert(service.Event(3,asr));assert(service.Event(3,final));assert(service.End(3));Wait([&]{return service.status()=="unsaved";});
 assert(!std::filesystem::is_empty(std::string(dir)+"/spool"));
 assert(service.Begin(4));Wait([&]{return count()>=7;});assert(service.Activate(4));
 assert(service.Event(4,Text(user,"Ben","ben-4")));
 {std::lock_guard<std::mutex> l(mu);assert(!actions.back().identity_pending && actions.back().greeting.find("Ben")!=std::string::npos);}
 {std::lock_guard<std::mutex> l(mu);assert(actions.back().instructions.find("dinosaurs")!=std::string::npos);assert(actions.back().instructions.find("AmyFixture")==std::string::npos);}
 assert(service.End(4));
 // While slow transport blocks only the background worker, producer stays bounded.
 unreachable=false;slow=true;assert(service.Begin(5));
 const auto start=std::chrono::steady_clock::now();int rejected=0;
 for(int i=0;i<1000;++i)if(!service.Event(5,asr))++rejected;
 assert(rejected>0);assert(std::chrono::steady_clock::now()-start<std::chrono::seconds(1));
 Wait([&]{return service.status()=="queue_full";});service.Stop();
 config::Memory disabled;disabled.cache_dir=std::string(dir)+"/off";int disabled_calls=0;
 memory::Service no_memory(disabled,[&](const std::string&,const Json&)->memory::Reply{++disabled_calls;return {};},[](memory::Action){assert(false);});
 no_memory.Start();assert(!no_memory.Begin(1));assert(!no_memory.Event(1,asr));no_memory.Stop();assert(disabled_calls==0);assert(!std::filesystem::exists(disabled.cache_dir));
 std::filesystem::remove_all(dir); // Test-created exact directory only, no device files.
}
