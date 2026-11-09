#ifdef NDEBUG
#error requires assertions
#endif
#include "memory/service.hpp"
#include <cassert>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>
using namespace xiaoai_plus;
using memory::Json;
using namespace std::chrono_literals;
int main() {
 for(const auto& label:{"ben","adult","exit","evil","timeout"}) {
  char dir[]="/tmp/p5f-identity-XXXXXX";assert(mkdtemp(dir));config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
  std::mutex mu;std::vector<memory::Action> actions;std::atomic<int> calls{0};
  memory::Service svc(cfg,[&](const std::string& path,const Json& body)->memory::Reply {
   if(path=="/device/snapshot")return {200,{{"generation",1},{"expires_at",std::time(nullptr)+86400},{"children",Json::array({
    Json{{"id","amy"},{"version",1},{"profile",{{"name","Amy"},{"aliases",Json::array()}}},{"summaries",Json::array()}},
    Json{{"id","ben"},{"version",1},{"profile",{{"name","Ben"},{"aliases",Json::array()}}},{"summaries",Json::array()}}})}}};
   if(path=="/device/identity") {
    ++calls;assert(body.size()==1 && body.contains("text"));
    if(std::string(label)=="timeout")std::this_thread::sleep_for(2500ms);
    return {200,{{"result",std::string(label)=="timeout"?"adult":label}}};
   }
   return {200,Json::object()};
  },[&](memory::Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(a);});
  auto wait=[&](size_t n){for(int i=0;i<500;++i){{std::lock_guard<std::mutex> l(mu);if(actions.size()>=n)return;}std::this_thread::sleep_for(10ms);}assert(false);};
  auto event=[](const char* text,const char* id){realtime::duplex::Event e;e.type="conversation.item.input_audio_transcription.completed";e.text=text;e.event_id=id;return e;};
  svc.Start();for(int i=0;i<500 && !svc.cache_revision();++i)std::this_thread::sleep_for(10ms);assert(svc.cache_revision());
  svc.Begin(1);wait(1);svc.Activate(1);const auto start=std::chrono::steady_clock::now();svc.Event(1,event("opaque","e1"));wait(2);
  const bool exit=std::string(label)=="exit";
  const bool retry=!exit && std::string(label)!="adult";
  {std::lock_guard<std::mutex> l(mu);const auto& a=actions.back();
   assert(a.identity_pending==retry);
   assert(a.greeting==(retry?"Hi! Are you Amy or Ben? Or a grown-up?":exit?"":"Hello! Let's chat."));
   assert(a.exit==exit && a.restart==!exit && a.after_reply==!exit);
   assert(a.instructions.find("Untrusted profile")==std::string::npos);
  }
  size_t expected=2;
  if(retry) {
   svc.Activate(1);svc.Event(1,event("opaque again","e2"));wait(++expected);
   {std::lock_guard<std::mutex> l(mu);assert(!actions.back().identity_pending && actions.back().greeting.find("Let's play! ")==0);}
  }
  if(!exit) {
   svc.Activate(1);svc.Event(1,event("tell me more","e3"));std::this_thread::sleep_for(700ms);
   {std::lock_guard<std::mutex> l(mu);assert(actions.size()==expected && calls==(retry?2:1));}
   svc.Event(1,event("我是 Amy","correction"));wait(expected+1);
   {std::lock_guard<std::mutex> l(mu);assert(actions.back().instructions.find("Amy")!=std::string::npos && actions.back().greeting.find("Hi, Amy!")==0);}
  }
  if(std::string(label)=="timeout")assert(std::chrono::steady_clock::now()-start<6000ms);
  svc.Stop();std::filesystem::remove_all(dir);
 }
}
