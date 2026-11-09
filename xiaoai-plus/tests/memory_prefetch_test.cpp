#ifdef NDEBUG
#error assertions required
#endif
#include "memory/service.hpp"
#include <cassert>
#include <filesystem>
#include <thread>
#include <unistd.h>
using namespace xiaoai_plus;
using memory::Json;
int main(){
 char dir[]="/tmp/prefetch-XXXXXX";assert(mkdtemp(dir));config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=dir;
 std::atomic<bool> blocked{true},entered{false};std::atomic<int> actions{0},reads{0},generation{0};
 memory::Service svc(cfg,[&](const std::string& path,const Json&)->memory::Reply{
   if(path=="/device/snapshot"){
     ++reads;entered=true;while(blocked)std::this_thread::sleep_for(std::chrono::milliseconds(10));
     return {200,{{"generation",generation.load()},{"expires_at",std::time(nullptr)+86400},{"children",Json::array()}}};
   }return {200,{{"ok",true}}};},[&](memory::Action){++actions;});
 auto wait=[&](auto f){for(int i=0;i<400 && !f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(f());};
 svc.Start();wait([&]{return entered.load();});auto start=std::chrono::steady_clock::now();
 assert(svc.Begin(1));wait([&]{return actions.load()==1;});
 assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(300));
 assert(reads==1);blocked=false;wait([&]{return svc.cache_revision()>0;});
 // A slow refresh cannot delay a new wake even while TLS/upload worker is occupied.
 blocked=true;entered=false;assert(svc.Prefetch());wait([&]{return entered.load();});
 start=std::chrono::steady_clock::now();assert(svc.Begin(2));wait([&]{return actions.load()==2;});
 assert(std::chrono::steady_clock::now()-start<std::chrono::milliseconds(300));
 generation=1;blocked=false;wait([&]{return actions.load()==3;}); // deletion abandons old context
 svc.Stop();std::filesystem::remove_all(dir);
}
