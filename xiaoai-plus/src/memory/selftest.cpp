#include "memory/selftest.hpp"
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>

namespace xiaoai_plus::memory {
namespace {
void Require(bool ok) { if (!ok) throw std::runtime_error("selftest_step_failed"); }
bool Wait(const std::function<bool()>& f, int seconds) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
  do { if(f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
  while(std::chrono::steady_clock::now()<end);
  return false;
}
realtime::duplex::Event Text(const char* type,const char* text,const char* id,
                           const char* item="",const char* response="",const char* question="") {
  realtime::duplex::Event e; e.type=type;e.text=text;e.event_id=id;e.item_id=item;
  e.response_id=response;e.question_id=question;return e;
}
Json FindChild(const Json& snap, const std::string& name) {
  for(const auto& c:snap.at("children"))if(c.at("profile").at("name")==name)return c;
  throw std::runtime_error("selftest_child_missing");
}
}
int Selftest(config::Memory cfg,Transport transport,std::ostream& out,int summary_seconds,int delete_seconds) {
  std::string cache;
  std::string local_sid,server_sid;
  Json upload;
  std::mutex upload_mu;
  try {
    Require(cfg.enabled);
    if(cfg.child_names.empty()) { out<<"selftest_child_missing\n"; return 1; }
    const auto& child_name=cfg.child_names.front();
    if(cfg.identity_question.empty())cfg.identity_question=IdentityQuestion(cfg.child_names);
    auto health=transport("/health",Json());Require(health.status==200 && health.body.value("status","")=="ok");
    auto snap=transport("/device/snapshot",Json());Require(snap.status==200);
    const auto child=FindChild(snap.body,child_name);
    out<<"SELFTEST_HEALTH_PASS\n"<<std::flush;
    cache=cfg.cache_dir+"-selftest-XXXXXX";
    std::vector<char> name(cache.begin(),cache.end());name.push_back(0);
    Require(mkdtemp(name.data())!=nullptr);cache=name.data();cfg.cache_dir=cache;
    out<<"SELFTEST_CACHE_CREATED path="<<cache<<"\n"<<std::flush;
    std::mutex mu;std::vector<Action> actions;
    auto count=[&]{std::lock_guard<std::mutex> l(mu);return actions.size();};
    auto action=[&](size_t i){std::lock_guard<std::mutex> l(mu);return actions.at(i);};
    // Only one upload attempt, even on timeout. Prefix marks it for the existing
    // administrative synthetic-session cleanup; production Service stays unchanged.
    Transport single_upload=[&](const std::string& path,const Json& body)->Reply {
      if(path!="/device/sessions")return transport(path,body);
      Json wire=body;
      {
        std::lock_guard<std::mutex> l(upload_mu);
        if(!server_sid.empty())return {};
        local_sid=body.at("session_id").get<std::string>();
        server_sid="p5b-validation-p5c2-"+local_sid;
        wire["session_id"]=server_sid;upload=wire;
        out<<"SELFTEST_UPLOAD_BEGIN session="<<server_sid<<"\n"<<std::flush;
      }
      auto r=transport(path,wire);
      if(r.status==200 && r.body.value("ack","")==server_sid)r.body["ack"]=local_sid;
      return r;
    };
    {
      Service svc(cfg,single_upload,[&](Action a){std::lock_guard<std::mutex> l(mu);actions.push_back(std::move(a));});
      svc.Start();Require(Wait([&]{return svc.cache_revision()>0;},30));Require(svc.Begin(1));Require(Wait([&]{return count()==1;},10));
      Require(action(0).greeting==cfg.identity_question && action(0).instructions.find(IdentityInstructions(cfg.child_names))==0);
      out<<"SELFTEST_IDENTITY_QUESTION_PASS\n"<<std::flush;
      Require(svc.Activate(1));
      Require(svc.Event(1,Text("conversation.item.input_audio_transcription.completed",child_name.c_str(),"identity","identity-item")));
      Require(Wait([&]{return count()==2;},10));
      Require(!action(1).greeting.empty() && action(1).after_reply && action(1).instructions.find(child_name)!=std::string::npos);
      out<<"SELFTEST_CHILD_PROFILE_INSTRUCTIONS_PASS\n"<<std::flush;
      Require(svc.Activate(1));
      Require(svc.Event(1,Text("conversation.item.input_audio_transcription.completed",
        "[SYNTHETIC P5c2 test, not a real child conversation] I drew a Triceratops with three horns. What does herbivore mean?",
        "synthetic-q","q1")));
      Require(svc.Event(1,Text("response.output_text.done",
        "[SYNTHETIC P5c2 assistant] A herbivore eats plants. What plant would you draw beside your dinosaur?",
        "synthetic-a","","r1","q1")));
      Require(svc.End(1));
      Require(Wait([&]{
        std::string id;{std::lock_guard<std::mutex> l(upload_mu);id=server_sid;}
        if(id.empty())return false;
        auto r=transport("/device/snapshot",Json());if(r.status!=200)return false;
        const auto amy=FindChild(r.body,child_name);
        for(const auto& s:amy.at("summaries"))if(s.value("session_id","")==id) {
          Require(s.value("status","")=="ark-v1");return true;
        }
        return false;
      },summary_seconds));
      const auto revision=svc.cache_revision();Require(svc.Prefetch());
      Require(Wait([&]{return svc.cache_revision()>revision;},30));
      Require(svc.Begin(2));Require(Wait([&]{return count()==3;},10));Require(svc.Activate(2));
      Require(action(2).greeting=="Hi "+child_name+", want to continue?" && action(2).instructions.find("Untrusted recent recap:")!=std::string::npos);
      Require(svc.End(2));svc.Stop();
      out<<"SELFTEST_SUMMARY_RESUME_PASS\n"<<std::flush;
    }
    // The host uses the restricted existing admin command, never parent secrets
    // on the speaker. Keep this one CLI invocation open until deletion readback.
    Require(Wait([&]{
      auto r=transport("/device/snapshot",Json());if(r.status!=200)return false;
      for(const auto& c:r.body.at("children"))for(const auto& s:c.at("summaries"))
        if(s.value("session_id","")==server_sid)return false;
      if(r.body.at("generation")==snap.body.at("generation"))return false;
      upload["generation"]=r.body.at("generation");
      Require(transport("/device/sessions",upload).status==410);
      Require(transport("/device/ack",Json{{"generation",r.body.at("generation")},{"status","ok"}}).status==200);
      return true;
    },delete_seconds));
    // Remove only files created under our exclusive mkdtemp directory.
    for(const auto& file:{"snapshot.json","active.json","snapshot.json.tmp","active.json.tmp"}) {
      const auto path=cache+"/"+file;if(std::filesystem::exists(path))Require(unlink(path.c_str())==0);
    }
    if(!local_sid.empty())for(const auto& suffix:{".json",".json.tmp"}) {
      const auto path=cache+"/spool/"+local_sid+suffix;
      if(std::filesystem::exists(path))Require(unlink(path.c_str())==0);
    }
    Require(rmdir((cache+"/spool").c_str())==0);Require(rmdir(cache.c_str())==0);
    out<<"SELFTEST_DELETE_REPLAY_LOCAL_CLEANUP_PASS\nSELFTEST_PASS\n"<<std::flush;
    return 0;
  } catch(...) {
    // No original exceptions, credentials, profiles, or transcripts in output.
    out<<"SELFTEST_FAIL (retain private test cache for cleanup)\n"<<std::flush;
    return 1;
  }
}
}
