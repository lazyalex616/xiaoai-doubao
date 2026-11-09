#ifdef NDEBUG
#error memory tests require active assert
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>
#include "memory/service.hpp"
using namespace xiaoai_plus;
using memory::Json;
void Wait(const std::function<bool()>& f) {
  for(int i=0;i<500 && !f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(f());
}
Json Children(bool deleted) {
  Json card={{"date","2026-10-23"},{"session_id","synthetic-ben-dinosaurs"},{"topic","Triceratops"},
    {"recap","Drew a dinosaur."},{"resume_question","What plant would your dinosaur eat?"},
    {"new_words",Json::array({"horn","tail","plant","ignored-fourth"})}};
  return Json::array({Json{{"id","ben"},{"version",1},{"profile",{{"name","BenFixture"},{"aliases",Json::array({"Ben"})},
    {"level","short sentences"},{"interests","drawing"},{"goals","ask questions"}}},
    {"summaries",deleted?Json::array():Json::array({card})},
    {"temporary",deleted?"":"Synthetic pending telescope conversation"}},
    Json{{"id","amy"},{"version",1},{"profile",{{"name","AmyFixture"},{"aliases",Json::array({"Amy"})}}},{"summaries",Json::array()}}});
}
void IdleDeletion() {
  char dir[]="/tmp/memory-idle-deletion-XXXXXX";assert(mkdtemp(dir));
  config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
  std::atomic<bool> deleted{false};std::mutex mu;std::vector<memory::Action> actions;
  memory::Service service(cfg,[&](const std::string& path,const Json&)->memory::Reply {
    if(path=="/device/snapshot")return {200,{{"generation",deleted?1:0},{"expires_at",std::time(nullptr)+86400},{"children",Children(deleted)}}};
    return {503,Json::object()};
  },[&](memory::Action a){std::lock_guard<std::mutex> lock(mu);actions.push_back(std::move(a));});
  auto count=[&]{std::lock_guard<std::mutex> lock(mu);return actions.size();};
  auto last=[&]{std::lock_guard<std::mutex> lock(mu);return actions.back();};
  auto hear=[&](uint64_t epoch,const char* text){realtime::duplex::Event e;
    e.type="conversation.item.input_audio_transcription.completed";e.text=text;e.event_id=text;assert(service.Event(epoch,e));};
  service.Start();Wait([&]{return service.cache_revision()>0;});
  assert(service.Begin(1));Wait([&]{return count()==1;});assert(service.Activate(1));
  hear(1,"I'm Ben");Wait([&]{return count()==2;});assert(service.Activate(1));
  hear(1,"We drew a Triceratops with a horn and tail.");
  assert(service.End(1));assert(service.Begin(2));Wait([&]{return count()==3;});
  assert(!last().identity_pending && last().greeting=="Hi BenFixture, want to continue?");
  assert(last().instructions.find("Untrusted recent user: We drew a Triceratops")!=std::string::npos);
  assert(service.Activate(2));assert(service.End(2));
  const auto revision=service.cache_revision();deleted=true;assert(service.Prefetch());
  Wait([&]{return service.cache_revision()>revision;});assert(count()==3);
  assert(service.Begin(3));Wait([&]{return count()==4;});
  assert(last().identity_pending && !last().greeting.empty());
  assert(last().instructions.find("Triceratops")==std::string::npos);
  assert(last().instructions.find("This is a short resume")==std::string::npos);
  assert(service.Activate(3));hear(3,"I am Ben");Wait([&]{return count()==5;});
  for(const char* text:{"Triceratops","horn, tail, plant","telescope","Untrusted recent"})
    assert(last().instructions.find(text)==std::string::npos);
  assert(last().instructions.find("already identified as Ben")!=std::string::npos);
  service.Stop();std::filesystem::remove_all(dir);
}
void RewakeIdentity() {
  for(const char* actor:{"I'm Amy","I'm a guest","I'm dad"}) {
    char dir[]="/tmp/memory-rewake-XXXXXX";assert(mkdtemp(dir));
    config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
    std::mutex mu;std::vector<memory::Action> actions;
    memory::Service service(cfg,[&](const std::string& path,const Json&)->memory::Reply {
      if(path=="/device/snapshot")return {200,{{"generation",0},{"expires_at",std::time(nullptr)+86400},{"children",Children(false)}}};
      return {503,Json::object()};
    },[&](memory::Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(std::move(a));});
    auto count=[&]{std::lock_guard<std::mutex> l(mu);return actions.size();};
    auto last=[&]{std::lock_guard<std::mutex> l(mu);return actions.back();};
    auto hear=[&](const char* id){realtime::duplex::Event e;e.type="conversation.item.input_audio_transcription.completed";
      e.text=actor;e.event_id=id;assert(service.Event(1,e));};
    service.Start();Wait([&]{return service.cache_revision()>0;});
    assert(service.Begin(1));Wait([&]{return count()==1;});assert(service.Activate(1));
    hear("first");Wait([&]{return count()==2;});
    const bool guest=std::string(actor)=="I'm a guest",child=std::string(actor)=="I'm Amy";
    if(guest) {
      assert(last().identity_pending && last().after_reply && last().greeting==cfg.identity_question);
      assert(service.Activate(1));hear("first"); // Duplicate final must not repeat the retry.
      hear("second");Wait([&]{return count()==3;});
      assert(!last().identity_pending && last().greeting.find("Let's play! ")==0);
    }else assert(last().greeting.find(child?"Hi, AmyFixture!":"Hello! Let's chat.")==0);
    assert(last().after_reply);assert(service.Activate(1));
    const auto expected=count()+1;
    // Both ends of this short gap are inside the unchanged ten-minute window.
    assert(service.End(1));assert(service.Begin(2));Wait([&]{return count()==expected;});
    assert(last().identity_pending==!child);
    assert(last().greeting==(child?"Hi AmyFixture, want to continue?":cfg.identity_question));
    assert(service.Activate(2));std::this_thread::sleep_for(std::chrono::milliseconds(50));
    assert(count()==expected); // Activation must not emit the opening a second time.
    service.Stop();std::filesystem::remove_all(dir);
  }
}
int main() {
  RewakeIdentity();
  IdleDeletion();
  auto child=Children(false)[0];
  for(const char* field:{"level","interests","goals"})child["profile"][field]=std::string(field)+std::string(500,'x');
  const auto bounded=memory::BuildInstructions(child);
  assert(bounded.size()<=memory::kInstructionsBudget && bounded.find("SAFETY FIRST")!=std::string::npos);
  for(const char* text:{"2026-10-23","Triceratops","What plant would your dinosaur eat?","horn, tail, plant",
                       "level=level","interests=interests","goals=goals","Follow topic changes; never force review"})
    assert(bounded.find(text)!=std::string::npos);
  assert(bounded.find("ignored-fourth")==std::string::npos);
  char dir[]="/tmp/memory-continuation-XXXXXX";assert(mkdtemp(dir));
  config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
  std::atomic<bool> deleted{false};std::mutex mu;std::vector<memory::Action> actions;
  memory::Service service(cfg,[&](const std::string& path,const Json&)->memory::Reply {
    if(path=="/device/snapshot")return {200,{{"generation",deleted?1:0},{"expires_at",std::time(nullptr)+86400},{"children",Children(deleted)}}};
    return {503,Json::object()};
  },[&](memory::Action a){std::lock_guard<std::mutex> lock(mu);actions.push_back(std::move(a));});
  auto count=[&]{std::lock_guard<std::mutex> lock(mu);return actions.size();};
  auto last=[&]{std::lock_guard<std::mutex> lock(mu);return actions.back();};
  auto hear=[&](uint64_t epoch,const char* text){realtime::duplex::Event e;
    e.type="conversation.item.input_audio_transcription.completed";e.text=text;e.event_id=text;assert(service.Event(epoch,e));};
  service.Start();Wait([&]{return service.cache_revision()>0;});
  // A fresh process with yesterday's card requires identity again, like a long gap.
  assert(service.Begin(1));Wait([&]{return count()==1;});
  assert(last().identity_pending && last().instructions.find("Triceratops")==std::string::npos);
  assert(service.Activate(1));hear(1,"I'm Ben");Wait([&]{return count()==2;});
  for(const char* text:{"Triceratops","horn, tail, plant","What plant would your dinosaur eat?","Synthetic pending telescope"})
    assert(last().instructions.find(text)!=std::string::npos);
  assert(last().instructions.find("AmyFixture")==std::string::npos);
  assert(service.Activate(1));assert(service.End(1));assert(service.Begin(2));Wait([&]{return count()==3;});
  assert(!last().identity_pending && last().greeting=="Hi BenFixture, want to continue?");
  assert(last().instructions.find("This is a short resume")!=std::string::npos);
  assert(service.Activate(2));hear(2,"I'm Amy");Wait([&]{return count()==4;});
  assert(last().instructions.find("Triceratops")==std::string::npos && last().instructions.find("horn, tail, plant")==std::string::npos);
  assert(service.Activate(2));hear(2,"I'm Ben");Wait([&]{return count()==5;});assert(service.Activate(2));
  deleted=true;assert(service.Prefetch());Wait([&]{return count()==6;});
  assert(last().identity_pending && last().instructions.find("Triceratops")==std::string::npos);
  assert(service.Activate(2));hear(2,"I am Ben");Wait([&]{return count()==7;});
  assert(last().instructions.find("Triceratops")==std::string::npos && last().instructions.find("telescope")==std::string::npos);
  service.Stop();std::filesystem::remove_all(dir);
}
