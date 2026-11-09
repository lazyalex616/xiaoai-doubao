#ifdef NDEBUG
#error recovery tests require active assert
#endif
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include "memory/service.hpp"
using namespace xiaoai_plus;
using memory::Json;
void Write(const std::string& p,const Json& j){std::ofstream(p)<<j.dump();chmod(p.c_str(),0600);}
void Wait(const std::function<bool()>& f){for(int i=0;i<600 && !f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(f());}
Json Snapshot(int gen,int64_t expiry){return {{"generation",gen},{"expires_at",expiry},{"children",Json::array({
  Json{{"id","amy"},{"version",1},{"profile",{{"name","AmyFixture"},{"aliases",Json::array({"Amy"})}}},{"summaries",Json::array({Json{{"topic","dinosaur"},{"recap","Amy likes dinosaurs"},{"resume_question","Which dinosaur?"},{"new_words",Json::array()}}})}}})}};}
Json Partial(){return {{"session_id",std::string(32,'a')},{"child_id","amy"},{"generation",0},{"profile_version",1},
  {"started_at",std::time(nullptr)},{"ended_at",std::time(nullptr)},{"complete",false},
  {"turns",Json::array({Json{{"seq",0},{"role","user"},{"text","dinosaurs"}}})}};}
realtime::duplex::Event User(const std::string& text){realtime::duplex::Event e;e.type="conversation.item.input_audio_transcription.completed";e.text=text;e.event_id=text;e.item_id=text;return e;}
int main(){
 char dir[]="/tmp/xiaoai-memory-recovery-XXXXXX";assert(mkdtemp(dir));
 config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;std::mutex mu;std::vector<memory::Action> actions;int uploads=0;
 auto action=[&](memory::Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(a);};
 auto count=[&]{std::lock_guard<std::mutex> l(mu);return actions.size();};
 auto snapshot=Snapshot(0,std::time(nullptr)+86400);Write(std::string(dir)+"/snapshot.json",snapshot);Write(std::string(dir)+"/active.json",Partial());
 std::filesystem::create_directory(std::string(dir)+"/spool");
 // Simulate a crash halfway through spool atomic-write, with durable active text.
 const auto orphan=std::string(dir)+"/spool/"+std::string(32,'a')+".json.tmp";
 std::ofstream(orphan)<<"{partial";chmod(orphan.c_str(),0600);
 {
  memory::Service service(cfg,[&](const std::string& path,const Json& body)->memory::Reply {
   if(path=="/device/snapshot")return {200,snapshot};
   if(path=="/device/sessions"){std::lock_guard<std::mutex> l(mu);++uploads;assert(body["complete"]==false);return {200,{{"ack",body["session_id"]}}};}
   return {200,{{"ok",true}}};},action);
  service.Start();Wait([&]{std::lock_guard<std::mutex> l(mu);return uploads==1 && std::filesystem::is_empty(std::string(dir)+"/spool");});service.Stop();
 }
 assert(Json::parse(std::ifstream(std::string(dir)+"/active.json")).empty());
 assert(!std::filesystem::exists(orphan));
 // Expired cache cannot inject old instructions even when server is unreachable.
 actions.clear();Write(std::string(dir)+"/snapshot.json",Snapshot(0,std::time(nullptr)-1));
 {
  memory::Service service(cfg,[](const std::string&,const Json&)->memory::Reply{return {};},action);
  service.Start();assert(service.Begin(1));Wait([&]{return count()==1;});assert(service.Activate(1));assert(service.Event(1,User("Amy")));Wait([&]{return count()==2;});
  {std::lock_guard<std::mutex> l(mu);assert(actions.back().instructions.find("dinosaurs")==std::string::npos);}
  assert(service.Activate(1));assert(service.Event(1,User("private guest chat")));assert(service.End(1));service.Stop();
 }
 assert(std::filesystem::is_empty(std::string(dir)+"/spool"));
 // A new server deletion generation removes both crash-active and queued text.
 actions.clear();Write(std::string(dir)+"/snapshot.json",Snapshot(0,std::time(nullptr)+86400));Write(std::string(dir)+"/active.json",Partial());
 {
  memory::Service service(cfg,[](const std::string& path,const Json&)->memory::Reply{
   if(path=="/device/snapshot")return {200,Snapshot(1,std::time(nullptr)+86400)};
   assert(path!="/device/sessions");return {200,{{"ok",true}}};},action);
  service.Start();Wait([&]{return service.cache_revision()>0;});assert(service.Begin(1));Wait([&]{return count()==1;});service.Stop();
 }
 assert(std::filesystem::is_empty(std::string(dir)+"/spool"));
 // Full spool preserves old files; a new child's dialogue still starts without recording.
 actions.clear();for(int i=0;i<8;i++){
  auto p=Partial();p["session_id"]=std::string(31,'b')+char('0'+i);p["generation"]=1;p["padding"]=std::string(240*1024,'x');
  Write(std::string(dir)+"/spool/"+p["session_id"].get<std::string>()+".json",p);
 }
 {
  memory::Service service(cfg,[](const std::string& path,const Json&)->memory::Reply{if(path=="/device/snapshot")return {200,Snapshot(1,std::time(nullptr)+86400)};return {};},action);
  service.Start();assert(service.Begin(1));Wait([&]{return count()==1;});assert(service.Activate(1));assert(service.Event(1,User("Amy")));Wait([&]{return count()==2;});assert(service.Activate(1));Wait([&]{return service.status()=="unsaved";});
  assert(service.Event(1,User("new conversation")));assert(service.End(1));service.Stop();
 }
 assert(std::distance(std::filesystem::directory_iterator(std::string(dir)+"/spool"),std::filesystem::directory_iterator{})==8);
 assert(Json::parse(std::ifstream(std::string(dir)+"/active.json")).empty());
 // Storage failure still permits a guest/adult conversation and sends parent status.
 std::filesystem::remove_all(std::string(dir)+"/spool");std::filesystem::create_directory(std::string(dir)+"/spool");
 Write(std::string(dir)+"/active.json",Json::object());chmod((std::string(dir)+"/active.json").c_str(),0644);
 actions.clear();std::atomic<bool> reported{false};
 {
  memory::Service service(cfg,[&](const std::string& path,const Json& body)->memory::Reply{
    if(path=="/device/snapshot")return {200,Snapshot(1,std::time(nullptr)+86400)};
    if(path=="/device/ack"){assert(body["status"]=="storage_error");reported=true;}
    assert(path!="/device/sessions");return {200,{{"ok",true}}};},action);
  service.Start();assert(service.Begin(1));Wait([&]{return count()==1 && reported.load();});
  assert(service.Activate(1));assert(service.Event(1,User("I'm daddy")));Wait([&]{return count()==2;});
  {std::lock_guard<std::mutex> l(mu);assert(actions.back().instructions.find("AmyFixture")==std::string::npos);assert(actions.back().instructions.find("dinosaurs")==std::string::npos);}
  service.Stop();
 }
 std::filesystem::remove_all(dir);
}
