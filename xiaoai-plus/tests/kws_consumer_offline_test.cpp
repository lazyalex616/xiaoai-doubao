#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "app/app.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using Clock=std::chrono::steady_clock;
class ControlledEngine final:public wakeup::IKwsEngine {
 public:
  std::optional<wakeup::KwsHit> AcceptPcm16(const uint8_t*,size_t,int,int,int) override {
    active=true;std::unique_lock<std::mutex> l(mu);++calls;entered=true;cv.notify_all();
    cv.wait(l,[&]{return !blocked;});
    if(error){active=false;throw std::runtime_error("controlled KWS failure");}
    auto label=std::move(hit);hit.clear();active=false;
    if(label.empty())return {};return wakeup::KwsHit{label};
  }
  void Reset() override {if(active)bad_reset=true;++resets;}
  void Block(std::string label={}) {std::lock_guard<std::mutex> l(mu);blocked=true;entered=false;hit=std::move(label);}
  void Release() {std::lock_guard<std::mutex> l(mu);blocked=false;cv.notify_all();}
  void WaitEntered(){std::unique_lock<std::mutex> l(mu);assert(cv.wait_for(l,3s,[&]{return entered;}));}
  void Push(std::string label) {std::lock_guard<std::mutex> l(mu);hit=std::move(label);}
  std::atomic<int> calls{0},resets{0};std::atomic<bool> active{false},bad_reset{false},error{false};
 private:std::mutex mu;std::condition_variable cv;bool blocked=false,entered=false;std::string hit;
};
template<class F> void Wait(F f){for(int i=0;i<600 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());}
namespace xiaoai_plus::app {
struct AppOfflineProbe {
 static void Run(const std::string& dir,const std::string& mode,int port,std::atomic<int>& sessions,std::atomic<int>& greetings) {
  config::Config cfg;cfg.realtime.protocol="v3";cfg.conversation.ordinary_barge_in=mode!="legacy";
  const bool session_failure=mode=="session-unavailable" || mode=="session-retry-success";
  const bool with_memory=mode=="positive-control" || mode=="native-queue" || session_failure;
  cfg.memory.enabled=with_memory;cfg.memory.cache_dir=dir+"/memory";cfg.conversation.usage_dir=dir+"/usage";
  cfg.realtime.api_key="loopback-only";cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);
  App app(cfg,false,false,false,cfg.wakeup.s32_shift,cfg.wakeup.input_channel);
  auto engine=std::make_shared<ControlledEngine>();app.kws_listener_->Close();
  std::atomic<bool> disarmed{false};std::string disarm_reason,requested_rephrase;
  if(session_failure) {
    app.gate_->Close();wakeup::Hooks hooks;
    hooks.on_arm=[&](const std::string& reason){app.OnArm(reason);};
    hooks.after_disarm_scoped=[&](const std::string& reason,uint64_t revision){
      {std::lock_guard<std::mutex> l(app.control_mu_);
        if(app.rephrase_requested_)requested_rephrase=app.rephrase_text_;}
      app.AfterDisarmScoped(reason,revision);disarm_reason=reason;disarmed=true;
    };
    app.gate_=std::make_unique<wakeup::Gate>(30s,hooks);
  }
  wakeup::LocalListener::Config lc;lc.gate=app.gate_.get();lc.trigger=app.trigger_.get();lc.kws_engine=engine;
  lc.sample_rate=16000;lc.min_trigger_interval_ms=0;lc.allow_playback_interrupt=true;lc.bounded_decisions=true;
  lc.on_end=[]{};app.kws_listener_=std::make_unique<wakeup::LocalListener>(lc);
  app.running_=true;
  if(with_memory) {
    app.memory_=std::make_unique<memory::Service>(cfg.memory,[](const std::string& path,const memory::Json&)->memory::Reply {
      if(path=="/device/snapshot")return {200,{{"generation",9},{"expires_at",std::time(nullptr)+3600},{"children",memory::Json::array()}}};
      return {200,memory::Json::object()};
    },[&](memory::Action action){app.QueueMemoryAction(std::move(action));});
    app.memory_->Start();Wait([&]{return app.memory_->cache_revision()>0;});assert(app.player_->Start());
  }
  if(mode=="legacy") {app.StartKwsConsumer();assert(!app.kws_consumer_thread_.joinable() && !app.kws_accepting_);app.Stop();std::cout<<"PASS ordinary off no consumer\n";return;}
  app.StartKwsConsumer();assert(app.StartRecorder());
  // Exercise actual cold start. Only real Stop/fork increments generation;
  // same-generation capture latch/200ms are never reset by this fixture.
  for(int i=0;i<600 && !app.capture_healthy_;++i) {
   bool fault;{std::lock_guard<std::mutex> l(app.control_mu_);fault=app.capture_restart_requested_;app.capture_restart_requested_=false;}
   if(fault)app.RestartBargeCapture();std::this_thread::sleep_for(10ms);
  }
  assert(app.capture_healthy_);const auto generation=app.recorder_->Generation();
  auto sample=[&]{std::lock_guard<std::mutex> l(app.barge_mu_);return app.capture_last_ns_;};
  auto producer_sample=[&]{std::lock_guard<std::mutex> l(app.kws_dispatch_mu_);return app.current_capture_stamp_.first_sample;};
  auto queue=[&]{std::lock_guard<std::mutex> l(app.kws_queue_mu_);assert(app.kws_queue_.size()<=3);return app.kws_queue_.size();};
  if(mode=="reader" || mode=="overflow" || mode=="oldgen" || mode=="reset") {
   const bool over=mode=="overflow";engine->Block(over?"豆包同学":"");engine->WaitEntered();
   const auto before=sample();const auto first_before=producer_sample();const int resets=engine->resets;
   if(mode=="reset")app.InvalidateKws();
   if(mode=="oldgen") {app.recorder_->Stop();assert(app.StartRecorder());assert(app.recorder_->Generation()>generation);}
   std::this_thread::sleep_for(over?450ms:230ms);
   const auto after=sample();
   std::cerr<<"READER actual stamp delta_ns="<<after-before<<" sample_delta="<<producer_sample()-first_before<<" gen="<<app.recorder_->Generation()<<" healthy="<<app.capture_healthy_<<"\n";
   if(mode!="oldgen")assert(app.recorder_->Generation()==generation && app.capture_healthy_ && producer_sample()-first_before>=2880);
   queue();assert(engine->resets==resets && !engine->bad_reset);
   engine->Release();Wait([&]{return !engine->active.load();});
   if(over || mode=="reset" || mode=="oldgen")Wait([&]{return engine->resets>resets;});
   assert(!app.start_requested_ && app.gate_->step()==wakeup::Step::kIdle && !engine->bad_reset);
   std::cout<<"PASS "<<mode<<" reader_advance_ns="<<after-before<<" samegen="<<(app.recorder_->Generation()==generation)<<" queue="<<queue()<<" reset_serial="<<!engine->bad_reset<<"\n";
  } else if(mode=="positive" || with_memory || mode=="old-disarm") {
   wakeup::Gate::DisarmCommit old;
   if(mode=="old-disarm") {assert(app.gate_->CommitWakeup("old"));old=app.gate_->CommitDisarm("old_timeout");assert(old);}
   assert(app.memory_epoch_==0 && app.identity_pending_);engine->Push("豆包同学");
   Wait([&]{std::lock_guard<std::mutex> l(app.control_mu_);return app.start_requested_;});
   App::KwsTicket ticket;{std::lock_guard<std::mutex> l(app.control_mu_);ticket=app.start_ticket_;}
   assert(ticket && app.KwsTicketCurrent(ticket) && app.gate_->step()==wakeup::Step::kActive);
   const auto before=sample();std::this_thread::sleep_for(650ms);
   assert(app.KwsTicketCurrent(ticket) && queue()==0 && sample()-before>=540000000ULL);
   if(mode=="old-disarm") {app.AfterDisarmScoped(old.reason,old.revision);assert(app.KwsTicketCurrent(ticket));assert(app.start_requested_ && !app.close_requested_);}
   if(mode=="native-queue") {
     app.OnNativeWake();app.control_thread_=std::thread([&]{app.ControlLoop();});
     Wait([&]{std::lock_guard<std::mutex> l(app.control_mu_);return !app.close_requested_ && !app.start_requested_;});
     std::this_thread::sleep_for(120ms);assert(app.memory_epoch_==0 && sessions==0 && !app.memory_switching_);
   } else if(mode=="positive-control" || session_failure) {
     app.control_thread_=std::thread([&]{app.ControlLoop();});
     if(mode=="session-unavailable") {
       Wait([&]{return disarmed.load();});
       assert(disarm_reason=="session_unavailable" && requested_rephrase=="豆包连不上了，请告诉爸爸妈妈。");
       assert(app.gate_->step()==wakeup::Step::kIdle && !app.memory_switching_ && !app.session_ready_);
       std::this_thread::sleep_for(1300ms);assert(sessions==2 && greetings==0);
       app.Stop();std::cout<<"PASS two rejected creates disarm session_unavailable, request parent feedback, no further creates\n";return;
     }
     Wait([&]{return app.session_ready_.load() && !app.memory_switching_.load();});
     if(session_failure) {
       Wait([&]{return greetings==1;});std::this_thread::sleep_for(1300ms);
       assert(sessions==2 && !disarmed && requested_rephrase.empty());
       {std::lock_guard<std::mutex> l(app.control_mu_);assert(!app.rephrase_requested_);}
     }
     assert(app.memory_epoch_==1 && sessions==(session_failure?2:1) && app.identity_pending_ && app.gate_->step()==wakeup::Step::kActive);
   }
   std::cout<<"PASS "<<mode<<" unknown epoch0 actual OnArm SessionStart enqueue preserved across 650ms preparation; sessions="<<sessions<<" epoch="<<app.memory_epoch_<<" staleDisarm="<<bool(old)<<"\n";
  } else if(mode=="native-hook" || mode=="stop-hook") {
   std::unique_lock<std::mutex> hook_barrier(app.control_mu_);engine->Push("豆包同学");
   Wait([&]{return app.gate_->step()==wakeup::Step::kActive;});
   std::thread invalidator([&]{if(mode=="native-hook")app.OnNativeWake();else app.Stop();});
   Wait([&]{std::lock_guard<std::mutex> l(app.kws_dispatch_mu_);return !app.kws_live_ticket_;});
   hook_barrier.unlock();invalidator.join();
   {std::lock_guard<std::mutex> l(app.control_mu_);assert(!app.start_requested_);}
   if(mode=="native-hook")assert(app.gate_->step()==wakeup::Step::kIdle && app.close_requested_);
   std::cout<<"PASS "<<mode<<" invalidation during actual OnArm before final enqueue rejects old wake and completes cleanup\n";
  } else if(mode=="recorder-error") {
   engine->Block("豆包同学");engine->WaitEntered();
   {std::ofstream marker(dir+"/producer-eof");marker<<1;}
   Wait([&]{return !app.running_.load();});assert(app.recorder_failed_);
   engine->Release();Wait([&]{return !engine->active.load();});
   std::this_thread::sleep_for(80ms);
   bool queued;{std::lock_guard<std::mutex> l(app.control_mu_);queued=app.start_requested_;}
   std::cerr<<"ACTUAL_EOF running="<<app.running_<<" accepting="<<app.kws_accepting_<<" queuedStart="<<queued<<" gateActive="<<(app.gate_->step()==wakeup::Step::kActive)<<"\n";
   assert(!app.kws_accepting_ && !app.capture_healthy_ && !queued && app.gate_->step()==wakeup::Step::kIdle);
   std::cout<<"PASS physical EOF invalidates in-flight hit before Gate/session claim; external Stop owns joins\n";
  } else if(mode=="worker-error") {
   engine->error=true;Wait([&]{return !app.running_.load();});assert(app.recorder_failed_ && !app.kws_accepting_);
   std::cout<<"PASS consumer exception FailWorker; parent Stop owns joins\n";
  } else assert(false);
  app.Stop();assert(!app.kws_consumer_thread_.joinable());
 }
};
}
int main(int argc,char** argv) {
 assert(argc==2);char temp[]="/tmp/xz-kws-consumer-XXXXXX";assert(mkdtemp(temp));std::string dir=temp;
 {std::ofstream f(dir+"/arecord");f<<"#!/usr/bin/env python3\nimport sys,time,os\nb=bytes(1440*16)\nwhile True:\n if os.path.exists('"<<dir<<"/producer-eof'): break\n sys.stdout.buffer.write(b);sys.stdout.buffer.flush();time.sleep(.09)\n";}
 assert(chmod((dir+"/arecord").c_str(),0700)==0);const std::string path=getenv("PATH");assert(setenv("PATH",(dir+":"+path).c_str(),1)==0);
 {std::ofstream f(dir+"/aplay");f<<"#!/usr/bin/env python3\nimport os\nwhile os.read(0,4096): pass\n";}assert(chmod((dir+"/aplay").c_str(),0700)==0);
 const std::string mode=argv[1];
 const int port=ix::getFreePort();std::atomic<int> sessions{0},greetings{0};ix::WebSocketServer server(port,"127.0.0.1");
 server.setOnClientMessageCallback([&](auto,ix::WebSocket& ws,const ix::WebSocketMessagePtr& m){
  if(m->type!=ix::WebSocketMessageType::Message)return;auto j=memory::Json::parse(m->str);const std::string type=j.at("type");
  if(type=="session.create") {
   const int attempt=++sessions;
   assert(j.dump().find("Only ask who is speaking")!=std::string::npos || j.dump().find("Identity is unknown")!=std::string::npos ||
     ((mode=="session-unavailable" || mode=="session-retry-success") && j.dump().find("App asks identity")!=std::string::npos));
   if(mode=="session-unavailable" || (mode=="session-retry-success" && attempt==1))
     ws.sendText(R"({"type":"error","code":45000292,"message":"quota exceeded for types: tokens_lifetime"})");
   else ws.sendText(R"({"type":"session.created","session":{"id":"local-initial-unknown"}})");
  }
  if(type=="session.finish")ws.sendText(R"({"type":"session.closed"})");
  if(type=="speech_text_buffer.commit") {++greetings;ws.sendText(R"({"type":"response.output_audio.delta","event_id":"a","question_id":"q","response_id":"r","delta":"AQI="})");ws.sendText(R"({"type":"response.output_audio.done","event_id":"d","question_id":"q","response_id":"r"})");ws.sendText(R"({"type":"response.done","response_id":"r","usage":{"total_tokens":1}})");}
 });assert(server.listen().first);server.start();
 app::AppOfflineProbe::Run(dir,mode,port,sessions,greetings);server.stop();std::filesystem::remove_all(dir);
}
