#ifdef NDEBUG
#error e2e requires active assert
#endif
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include "memory/service.hpp"
using namespace xiaoai_plus;
using memory::Json;
void Wait(const std::function<bool()>& f){for(int i=0;i<1500 && !f();++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(f());}
int main(int argc,char** argv){
 assert(argc==5);
 config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.server_url=argv[1];cfg.cache_dir=argv[3];
 std::ifstream secret(argv[2]);secret>>cfg.device_secret;
 auto transport=memory::HttpsTransport(cfg,argv[4]);
 auto snapshot=transport("/device/snapshot",Json());assert(snapshot.status==200);
 auto invalid=cfg;invalid.server_url="http://host.docker.internal:64105";assert(memory::HttpsTransport(invalid,argv[4])("/device/snapshot",Json()).status==0);
 assert(memory::HttpsTransport(cfg,"/missing/ca.pem")("/device/snapshot",Json()).status==0);
 invalid.server_url="https://127.0.0.1:64105"; // network failure and/or wrong SAN must fail closed
 assert(memory::HttpsTransport(invalid,argv[4])("/device/snapshot",Json()).status==0);
 std::mutex mu;std::vector<memory::Action> actions;
 memory::Service service(cfg,transport,[&](memory::Action a){std::lock_guard<std::mutex> lock(mu);actions.push_back(a);});
 auto count=[&]{std::lock_guard<std::mutex> lock(mu);return actions.size();};
 auto event=[](const char* type,const char* text,const char* id,const char* item,const char* response,const char* question){realtime::duplex::Event e;e.type=type;e.text=text;e.event_id=id;e.item_id=item;e.response_id=response;e.question_id=question;return e;};
 service.Start();assert(service.Begin(1));Wait([&]{return count()==1;});assert(service.Activate(1));
 assert(service.Event(1,event("conversation.item.input_audio_transcription.completed","Amy","identity","i","","")));Wait([&]{return count()==2;});assert(service.Activate(1));
 assert(service.Event(1,event("conversation.item.input_audio_transcription.completed","I like dinosaurs.","asr-final","q1","","")));
 assert(service.Event(1,event("response.output_text.done","Which dinosaur do you like?","answer-final","","r1","q1")));assert(service.End(1));
 Wait([&]{auto r=transport("/device/snapshot",Json());if(r.status!=200)return false;for(const auto& c:r.body["children"])if(c["profile"]["name"]=="Amy" && !c["summaries"].empty())return true;return false;});
 assert(service.Begin(2));Wait([&]{return count()==3;});assert(service.Activate(2));
 assert(service.Event(2,event("conversation.item.input_audio_transcription.completed","Amy","identity-2","i2","","")));Wait([&]{return count()==4;});
 {std::lock_guard<std::mutex> lock(mu);assert(actions.back().instructions.find("dinosaurs")!=std::string::npos);assert(actions.back().instructions.size()<=memory::kInstructionsBudget);assert(actions[2].instructions.find("dinosaurs")==std::string::npos);}
 assert(service.End(2));service.Stop();
 std::cout<<"PASS C++ simulated device -> verified local HTTPS -> SQLite -> mock summary -> next child instructions (<=memory::kInstructionsBudget-byte conservative budget)\n";
}
