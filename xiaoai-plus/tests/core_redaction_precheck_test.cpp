#ifdef NDEBUG
#error assertions required
#endif
#include "memory/service.hpp"
#include <cassert>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
using namespace xiaoai_plus;
using memory::Json;
using Clock = std::chrono::steady_clock;
struct Measurement {
  Clock::time_point wall=Clock::now();
  std::clock_t cpu=std::clock();
  Json End() const {
    return {{"wall_ms",std::chrono::duration<double,std::milli>(Clock::now()-wall).count()},
            {"process_cpu_ms",(std::clock()-cpu)*1000.0/CLOCKS_PER_SEC}};
  }
};
void Emit(const std::string& name,const std::string& output,const Measurement& measured) {
  auto row=measured.End();row["case"]=name;row["output"]=output;row["bytes"]=output.size();
  std::cout<<row.dump()<<std::endl;
}
void CoreCases() {
  const std::vector<std::pair<std::string,std::string>> cases={
    {"empty",""},{"neutral_ascii","English practice plus punctuation _+.- ()"},
    {"unicode_without_ascii_digits","中文练习 😀 café １２３４５６７８９ ١٢٣٤٥٦٧٨٩"},
    {"email_without_digits","abc.def+tag\100example.test"},
    {"email_with_digits","a1\100example2.test"},
    {"email_and_phone","abc\100demo.test +1 (234) 567-8901"},
    {"multiple_emails","a\100one.test / b+tag\100two.test"},
    {"at_without_match","@ / x@ / x@x.c / x@@example.test"},
    {"digits_without_match","1 / 12345678 / 2026-10"},
    {"phone_minimum_middle","1-------2"},
    {"phone_with_spaces","1 234 567 890"},
    {"phone_inside_unicode","你好123456789再见"},
    {"english_secret","My password hidden phrase. my school demo!"},
    {"chinese_secret","密码合成秘密，学校合成校名。API-Key demo!"},
    {"mixed_order","My phone abc\100demo.test and +1 (234) 567-8901. 地址合成内容。"},
    {"embedded_nul",std::string("a\0b\100demo.test",13)}
  };
  for(const auto& item:cases) {
    Measurement measured;auto output=memory::Redact(item.second);
    if(item.first=="email_without_digits" || item.first=="email_with_digits" ||
       item.first=="phone_minimum_middle" || item.first=="phone_with_spaces")assert(output=="[private]");
    if(item.first=="unicode_without_ascii_digits" || item.first=="neutral_ascii")assert(output==item.second);
    if(item.first=="email_and_phone")assert(output=="[private] [private]");
    Emit(item.first,output,measured);
  }
  for(const auto n:{10000,16427}) {
    auto text=std::string(n,'x');Measurement measured;auto output=memory::Redact(text);
    assert(output==text);Emit("neutral_long_"+std::to_string(n),output,measured);
  }
  Json child={{"profile",{{"name","Amy"},{"aliases",Json::array()},
                         {"interests",std::string(286,'x')+"abc\100demo.test +1 (234) 567-8901"}}},
              {"temporary",std::string(115,'x')+"abc\100demo.test"},
              {"summaries",Json::array({Json{{"recap",std::string(125,'x')+"abc\100demo.test"},
                                            {"resume_question",std::string(155,'x')+"abc\100demo.test"}}})}};
  Measurement measured;auto output=memory::BuildInstructions(child);
  assert(output.size()<=memory::kInstructionsBudget && output.find("SAFETY FIRST")!=std::string::npos);
  assert(output.find("abc@demo") == std::string::npos && output.find("234") == std::string::npos);
  Emit("redaction_before_budget",output,measured);
  child={{"profile",{{"name","Amy"},{"aliases",Json::array()},{"interests",std::string(283,'x')+"😀中文😀"}}},
         {"temporary",std::string(114,'x')+"😀中文😀"},{"summaries",Json::array()}};
  Measurement utf8;output=memory::BuildInstructions(child);
  assert(output.size()<=memory::kInstructionsBudget && output.find("SAFETY FIRST")!=std::string::npos);
  (void)Json(output).dump(-1,' ',false,Json::error_handler_t::strict);
  Emit("utf8_budget_boundary",output,utf8);
}
bool Wait(const std::function<bool()>& condition) {
  const auto deadline=Clock::now()+std::chrono::seconds(40);
  while(Clock::now()<deadline) {if(condition())return true;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
  return condition();
}
Json SyntheticChild(bool profile) {
  return {{"id","amy"},{"version",1},
          {"profile",{{"name","Amy"},{"aliases",Json::array()},
                      {"interests",profile?std::string(10000,'x'):"synthetic practice"}}},
          {"summaries",Json::array()}};
}
void ServiceCase(const std::string& mode,const std::string& cache) {
  assert(!std::filesystem::exists(cache));
  config::Memory cfg;cfg.child_names={"Amy","Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=cache;
  std::mutex mu;std::vector<memory::Action> actions;
  Json child=SyntheticChild(mode=="profile");
  memory::Service service(cfg,[&](const std::string& path,const Json&)->memory::Reply {
    if(path=="/device/snapshot") {
      Json snapshot={{"generation",1},{"expires_at",std::time(nullptr)+86400},{"children",Json::array({child})}};
      std::clog<<"SYNTHETIC snapshot_bytes="<<snapshot.dump().size()<<std::endl;
      return {200,snapshot};
    }
    if(path=="/device/session")return {503,Json::object()};
    return {200,{{"ok",true}}};
  },[&](memory::Action action){std::lock_guard<std::mutex> lock(mu);actions.push_back(std::move(action));});
  auto count=[&](){std::lock_guard<std::mutex> lock(mu);return actions.size();};
  auto action=[&](size_t index){std::lock_guard<std::mutex> lock(mu);return actions.at(index);};
  auto event=[&](uint64_t epoch,const std::string& text,const std::string& id,
                 const std::string& item,const std::string& question,const std::string& response,bool user){
    realtime::duplex::Event e;e.type=user?"conversation.item.input_audio_transcription.completed":"response.output_text.done";
    e.text=text;e.event_id=id;e.item_id=item;e.question_id=question;e.response_id=response;
    assert(service.Event(epoch,e));
  };
  service.Start();assert(Wait([&](){return service.cache_revision()>0;}));
  std::clog<<"VALID_SNAPSHOT_APPLIED"<<std::endl;
  assert(service.Begin(1));assert(Wait([&](){return count()==1;}));
  assert(action(0).identity_pending && action(0).instructions==memory::IdentityInstructions({"Amy","Ben"}));
  assert(action(0).instructions.find("Untrusted profile:")==std::string::npos);
  assert(service.Activate(1));
  Measurement profile;event(1,"Amy","identity-one","identity-item","","",true);
  assert(Wait([&](){return count()==2;}));
  assert(!action(1).identity_pending && action(1).greeting.find("Hi, Amy!")==0);
  assert(action(1).instructions.size()<=memory::kInstructionsBudget);
  Emit(mode+"_resolved_once",action(1).instructions,profile);
  if(mode=="qa") {
    assert(service.Activate(1));
    for(int i=0;i<2;++i) {
      event(1,std::string(4096,char('q'+i)),"user-"+std::to_string(i),"item-"+std::to_string(i),"","",true);
      event(1,std::string(4096,char('a'+i)),"answer-"+std::to_string(i),"","item-"+std::to_string(i),"response-"+std::to_string(i),false);
    }
    assert(service.End(1,true));assert(service.Begin(2));
    assert(Wait([&](){return count()==3;}));
    assert(action(2).identity_pending && action(2).instructions==memory::IdentityInstructions({"Amy","Ben"}));
    assert(action(2).instructions.find("Temporary continuation:")==std::string::npos);
    Json saved;size_t files=0;
    for(const auto& item:std::filesystem::directory_iterator(cache+"/spool")) {
      std::ifstream file(item.path());file>>saved;++files;
    }
    assert(files==1 && saved.at("child_id")=="amy" && saved.at("generation")==1);
    assert(saved.at("turns").size()==4);
    std::string temporary="Unsent temporary continuation: ";
    for(size_t i=0;i<4;i+=2) {
      assert(saved["turns"][i]["role"]=="user" && saved["turns"][i+1]["role"]=="assistant");
      assert(saved["turns"][i]["item_id"]==saved["turns"][i+1]["question_id"]);
      assert(saved["turns"][i]["text"].get<std::string>().size()==4096 && saved["turns"][i+1]["text"].get<std::string>().size()==4096);
      temporary+=saved["turns"][i]["text"].get<std::string>()+" / "+saved["turns"][i+1]["text"].get<std::string>()+" | ";
    }
    assert(temporary.size()==16427);std::clog<<"SYNTHETIC linked_pairs=2 temporary_bytes="<<temporary.size()<<std::endl;
    assert(service.Activate(2));Measurement qa;
    event(2,"Amy","identity-two","identity-item-two","","",true);
    assert(Wait([&](){return count()==4;}));auto resolved=action(3);
    assert(!resolved.identity_pending && resolved.greeting.find("Hi, Amy!")==0);
    assert(resolved.instructions.find("Temporary continuation: Unsent temporary continuation: "+std::string(20,'q'))!=std::string::npos);
    assert(resolved.instructions.size()<=memory::kInstructionsBudget);
    Emit("qa_16427_resolved_once",resolved.instructions,qa);
    child["temporary"]=temporary;
    assert(resolved.instructions==memory::BuildInstructions(child));
  }
  service.Stop();assert(count()==(mode=="qa"?4:2));
}
int main(int argc,char** argv) {
  if(argc==1 || (argc==2 && std::string(argv[1])=="core")){CoreCases();return 0;}
  assert(argc==3 && (std::string(argv[1])=="profile" || std::string(argv[1])=="qa"));
  ServiceCase(argv[1],argv[2]);return 0;
}
