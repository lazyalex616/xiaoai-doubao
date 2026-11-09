#ifdef NDEBUG
#error requires active assertions
#endif
#include <cassert>
#include <iostream>
#include <ctime>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include "memory/selftest.hpp"
using namespace xiaoai_plus;
using memory::Json;
int main(int argc, char** argv) {
  char dir[]="/tmp/memory-selftest-test-XXXXXX";assert(mkdtemp(dir));
  config::Memory cfg;cfg.child_names={argc>1?argv[1]:"Amy",argc>2?argv[2]:"Ben"};cfg.identity_question=memory::IdentityQuestion(cfg.child_names);cfg.enabled=true;cfg.cache_dir=std::string(dir)+"/memory";
  std::mutex mu;Json uploaded;int attempts=0;int summary_reads=0;bool deleted=false;
  std::ostringstream output;
  auto transport=[&](const std::string& path,const Json& body)->memory::Reply {
    std::lock_guard<std::mutex> l(mu);
    if(path=="/health")return {200,{{"status","ok"}}};
    if(!uploaded.is_null() && path=="/device/snapshot" && ++summary_reads>=6)deleted=true;
    if(path=="/device/snapshot") {
      Json summaries=Json::array();
      if(!uploaded.is_null() && !deleted)summaries.push_back({{"session_id",uploaded["session_id"]},{"status","ark-v1"},{"recap","Synthetic dinosaur drawing."},{"resume_question","Which plant?"}});
      return {200,{{"generation",deleted?2:1},{"expires_at",std::time(nullptr)+86400},{"children",Json::array({
        Json{{"id","test-child"},{"profile",{{"name",cfg.child_names.front()},{"aliases",Json::array({cfg.child_names.front()})}}},{"version",1},{"summaries",summaries}}})}}};
    }
    if(path=="/device/ack")return {200,{{"ok",true}}};
    assert(path=="/device/sessions");
    if(deleted)return {410,{{"error","deleted"}}};
    ++attempts;uploaded=body;assert(body["session_id"].get<std::string>().rfind("p5b-validation-p5c2-",0)==0);
    assert(body["turns"][0]["text"].get<std::string>().find("SYNTHETIC")!=std::string::npos);
    return {200,{{"ack",body["session_id"]}}};
  };
  const auto result=memory::Selftest(cfg,transport,output,10,10);if(result)std::cerr<<output.str();assert(result==0);
  assert(attempts==1);assert(output.str().find("SELFTEST_PASS")!=std::string::npos);
  assert(output.str().find("three horns")==std::string::npos);
  assert(rmdir(dir)==0);
  cfg.child_names.clear();std::ostringstream missing;
  assert(memory::Selftest(cfg,transport,missing,1,1)==1);
  assert(missing.str().find("selftest_child_missing")!=std::string::npos);
  cfg.enabled=false;std::ostringstream disabled;
  assert(memory::Selftest(cfg,transport,disabled,1,1)==1);
  assert(attempts==1);
}
