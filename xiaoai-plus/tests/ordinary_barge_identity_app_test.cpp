#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "app/app.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using J=memory::Json;
template<class F> void Wait(F f){for(int i=0;i<800 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());}
template<class F> memory::ResumeResult Reply(F f){std::promise<memory::ResumeResult> p;auto r=p.get_future();assert(f([&](auto v){p.set_value(std::move(v));}));assert(r.wait_for(3s)==std::future_status::ready);return r.get();}
namespace xiaoai_plus::app {
struct AppOfflineProbe {
  static void Run(const std::string& dir,int port,const std::string& name,std::atomic<int>& sessions,
      std::atomic<int>& name_sent,std::atomic<bool>& allow_name,std::atomic<bool>& next_allowed,std::atomic<int>& next_sent,
      std::atomic<bool>& disconnect,std::atomic<int>& greetings,bool interrupt_identity,bool privacy) {
    const bool adult=name.rfind("我是",0)==0;
    config::Config cfg;cfg.realtime.protocol="v3";cfg.realtime.api_key="loopback-only";cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);
    cfg.conversation.usage_dir=dir+"/usage";cfg.conversation.ordinary_barge_in=true;cfg.memory.enabled=true;cfg.memory.cache_dir=dir+"/memory";
    App app(cfg,false,false,false,cfg.wakeup.s32_shift,cfg.wakeup.input_channel);app.kws_listener_->Close();
    app.memory_epoch_=42;app.running_=true;std::atomic<int> identity_calls{0};
    app.memory_=std::make_unique<memory::Service>(cfg.memory,[&](const std::string& path,const J&)->memory::Reply {
      if(path=="/device/snapshot")return {200,{{"generation",9},{"expires_at",std::time(nullptr)+3600},
        {"children",J::array({J{{"id","amy"},{"version",4},{"profile",{{"name","Amy"},{"aliases",J::array({"Amy"})}}},{"summaries",J::array({J{{"recap","AMY_PRIVATE_TEST_HISTORY"}}})}},
          J{{"id","ben"},{"version",4},{"profile",{{"name","Ben"},{"aliases",J::array({"Ben"})}}},{"summaries",J::array({J{{"recap","BEN_PRIVATE_TEST_HISTORY"}}})}}})}}};
      if(path=="/device/identity"){++identity_calls;return {200,{{"result",adult?"adult":"evil"}}};}
      return {200,J::object()};
    },[&](memory::Action a){app.QueueMemoryAction(std::move(a));});
    app.memory_->Start();Wait([&]{return app.memory_->cache_revision()>0;});
    assert(app.player_->Start());app.control_thread_=std::thread([&]{app.ControlLoop();});
    assert(app.trigger_->Allows("豆包同学") && !app.trigger_->FireFromText("豆包同学"));
    assert(sessions==0 && app.gate_->step()==wakeup::Step::kIdle);
    assert(app.StartRecorder());
    // Real startup leaves capture running before the human arms a session.
    // A late cold-start delivery must not force a recorder restart.
    Wait([&]{std::lock_guard<std::mutex> l(app.barge_mu_);return app.capture_healthy_.load() && app.capture_last_generation_==app.recorder_->Generation() && app.capture_last_generation_>app.capture_fault_generation_;});
    if(std::filesystem::exists(dir+"/cold-fault-once"))assert(app.recorder_->Generation()==1 && app.capture_restart_attempts_==0);
    assert(app.trigger_->FireFromText("豆包同学"));
    // The real OnArm/ControlLoop is the sole Begin/epoch owner; no test Begin.
    Wait([&]{return sessions==1 && app.session_ready_.load() && app.identity_pending_.load();});
    Wait([&]{std::lock_guard<std::mutex> l(app.mu_);return app.is_ai_speaking_;});
    std::this_thread::sleep_for(600ms);
    const auto initial=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
    assert(initial.approved && initial.identity.kind=="unknown");
    const realtime::IdentitySessionOrigin old_legacy(initial.identity.epoch,initial.identity.revision,app.identity_session_sequence_);
    const auto prompt_generation=app.player_->Generation();
    {std::ofstream phase(dir+"/speech-phase");phase<<1;}
    Wait([&]{std::lock_guard<std::mutex> l(app.barge_mu_);return app.fresh_mode_ && app.fresh_granted_ && app.fresh_permit_.valid();});
    realtime::TransportEnvelope unknown_origin;{std::lock_guard<std::mutex> l(app.barge_mu_);unknown_origin=app.fresh_envelope_;}
    {std::ofstream phase(dir+"/speech-phase");phase<<0;}
    std::this_thread::sleep_for(350ms);
    allow_name=true;Wait([&]{return name_sent.load()==1;});
    Wait([&]{std::lock_guard<std::mutex> l(app.control_mu_);return !app.memory_actions_.empty() && app.memory_actions_.front().after_reply;});
    const auto reply_generation=app.player_->Generation();
    std::this_thread::sleep_for(250ms);
    assert(sessions==2 && app.player_->Generation()==reply_generation && !app.memory_switching_.load());
    assert(greetings==1); // Only the initial question; no local follow-up racing the cloud reply.
    if(interrupt_identity) {
      next_allowed=true;{std::ofstream phase(dir+"/speech-phase");phase<<2;}
      Wait([&]{return sessions==3 && next_sent==1;});
      const auto interrupted=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
      assert(interrupted.approved && interrupted.identity.child_id=="amy" && greetings==1);
      {std::lock_guard<std::mutex> l(app.control_mu_);assert(app.memory_actions_.empty());}
      app.Stop();std::cout<<"PASS actual near-end interruption during pending identity keeps captured PCM and grants only Amy context\n";return;
    }
    app.fresh_client_->HandleTextBound(unknown_origin,R"({"type":"response.output_audio.done","event_id":"name-d","question_id":"name-input","response_id":"name-r"})");
    app.fresh_client_->HandleTextBound(unknown_origin,R"({"type":"response.done","response_id":"name-r","usage":{"total_tokens":1}})");
    std::this_thread::sleep_for(150ms);assert(sessions==2);
    Wait([&]{return sessions>=3 && (!app.identity_pending_.load()) && !app.memory_switching_.load() && app.session_ready_.load();});
    const auto identified=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
    std::cerr<<"identified approved="<<identified.approved<<" reason="<<identified.reason<<" kind="<<identified.identity.kind<<" status="<<app.memory_->status()<<"\n";
    assert(std::filesystem::exists(dir+"/tail-drained"));
    assert(identified.approved && identified.identity.kind==(name=="unclear"?"guest":adult?"adult":"child"));
    if(!adult && name!="unclear")assert(identified.identity.child_id==(name=="Amy"?"amy":"ben"));
    else assert(identified.identity.child_id.empty());
    assert(app.player_->Generation()>prompt_generation && app.gate_->step()==wakeup::Step::kActive);
    {std::lock_guard<std::mutex> l(app.barge_mu_);
      assert(!app.fresh_mode_ && !app.fresh_permit_.valid() && app.barge_pcm_.empty());}
    const auto current_generation=app.player_->Generation();
    app.fresh_client_->HandleTextBound(unknown_origin,R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"late-name","item_id":"name-input","transcript":"Amy"})");
    app.fresh_client_->HandleTextBound(unknown_origin,R"({"type":"response.output_audio.delta","event_id":"late-unknown-a","question_id":"late-unknown-q","response_id":"late-unknown-r","delta":"AQI="})");
    assert(app.player_->Generation()==current_generation);
    realtime::duplex::Session p;p.Create("fixture","fixture");p.Receive(R"({"type":"session.created","session":{"id":"fixture"}})");
    const auto old_event=p.Receive(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"late-legacy","item_id":"late-legacy","transcript":"I am an unregistered person"})");
    assert(app.memory_->IdentitySessionEvent(old_legacy,old_event));
    assert(app.memory_->Event(app.memory_epoch_.load(),old_event));
    auto written=[&]{size_t bytes=0;std::ifstream f(dir+"/written-bytes");f>>bytes;return bytes;};
    Wait([&]{return written()>=4;}); // Prompt plus natural identity reply reached the actual aplay pipe.
    const auto before_next=written();
    assert(!app.client_->StartSession(10ms,"stale origin must not reuse active WS",old_legacy));
    const auto stale=app.fresh_client_->InspectFreshUsage();
    assert(!stale.storage_unknown && !stale.attribution_unknown && !stale.missing_totals);
    if((!adult && name!="unclear") || name=="我是他爹")assert(identity_calls==0); // Direct original registered-name rules.
    if(name=="unclear") {
      assert(!app.identity_pending_.load() && app.session_ready_.load() && app.gate_->step()==wakeup::Step::kActive);
      app.Stop();std::cout<<"PASS actual App unclear-name safe no-profile guest after exactly one question and listening, old origins denied; no real cloud ASR claim\n";return;
    }
    if(privacy) {
      // Pause only the test control consumer: privacy must cut playback before
      // session teardown/network work has any chance to run.
      {std::lock_guard<std::mutex> l(app.control_mu_);app.control_stop_=true;}
      app.control_cv_.notify_one();app.control_thread_.join();
      {std::lock_guard<std::mutex> l(app.control_mu_);app.control_stop_=false;}
      {std::lock_guard<std::mutex> l(app.mu_);app.reply_pending_=true;app.is_ai_speaking_=true;}
      const auto generation=app.player_->Generation();
      const realtime::IdentitySessionOrigin current(app.memory_epoch_.load(),identified.identity.revision,app.identity_session_sequence_);
      auto correction=old_event;correction.event_id="privacy-switch";correction.text="我是 Ben";
      assert(app.memory_->IdentitySessionEvent(current,correction));
      Wait([&]{std::lock_guard<std::mutex> l(app.control_mu_);return !app.memory_actions_.empty();});
      {std::lock_guard<std::mutex> l(app.control_mu_);const auto& a=app.memory_actions_.front();
        assert(!a.after_reply && a.instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos && a.instructions.find("BEN_PRIVATE_TEST_HISTORY")!=std::string::npos);}
      assert(app.player_->Generation()>generation && !app.legacy_effects_live_.load() && app.memory_switching_.load());
      const auto stopped=app.player_->Generation();app.OnAudio(std::vector<uint8_t>(640,0));
      assert(app.player_->Generation()==stopped);
      app.control_thread_=std::thread([&]{app.ControlLoop();});
      Wait([&]{return sessions==4 && app.session_ready_.load() && !app.memory_switching_.load();});
      const auto switched=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
      assert(switched.approved && switched.identity.child_id=="ben" && switched.instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos);
      app.Stop();std::cout<<"PASS actual App child handover interrupts player before control/network work, rejects old output and starts clean Ben context\n";return;
    }
    // A delayed producer and an actual overrun use the real capture/control
    // path while the identified legacy WS stays open; neither asks for a wake.
    for(const char* fault : {"gap", "overrun"}) {
      const auto generation=app.recorder_->Generation();
      uint64_t before=0;{std::lock_guard<std::mutex> l(app.barge_mu_);before=app.capture_last_ns_;}
      {std::ofstream request(dir+"/capture-fault");request<<fault;}
      Wait([&]{return !std::filesystem::exists(dir+"/capture-fault");});
      if(std::string(fault)=="overrun")Wait([&]{return app.recorder_->Generation()>generation;});
      else std::this_thread::sleep_for(450ms);
      Wait([&]{std::lock_guard<std::mutex> l(app.barge_mu_);return app.capture_healthy_.load() && app.capture_last_ns_>before;});
      assert(app.gate_->step()==wakeup::Step::kActive && app.session_ready_.load() && sessions==3);
      if(std::string(fault)=="gap")assert(app.recorder_->Generation()==generation);
      {std::lock_guard<std::mutex> l(app.control_mu_);assert(!app.rephrase_requested_);}
    }
    next_allowed=true;{std::ofstream phase(dir+"/speech-phase");phase<<2;}
    Wait([&]{return next_sent.load()==1 && written()>=before_next+2;});
    Wait([&]{std::lock_guard<std::mutex> l(app.mu_);return !app.is_ai_speaking_ && app.pending_playback_chunks_==0;});
    const auto continued=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
    assert(continued.approved && continued.identity.kind==identified.identity.kind && continued.identity.child_id==identified.identity.child_id);
    if(!adult) {
      Wait([&]{std::ifstream f(cfg.memory.cache_dir+"/active.json");J j;f>>j;return j.contains("turns") && j["turns"].size()==2;});
      std::ifstream f(cfg.memory.cache_dir+"/active.json");J j;f>>j;assert(j["child_id"]==identified.identity.child_id && j["turns"][0]["text"]=="Tell me an English joke" && j["turns"][1]["text"]=="fixture English answer");
    }
    if(adult) {
      assert(continued.instructions.find("PRIVATE_TEST_HISTORY")==std::string::npos && greetings==1);
      app.Stop();std::cout<<"PASS direct adult self-report, natural next turn, no child profile or extra identity question\\n";return;
    }
    assert(app.gate_->step()==wakeup::Step::kActive);
    const auto epoch=app.memory_epoch_.load();const int before_recovery=sessions,hello_count=greetings;
    assert(continued.instructions.find("Tell me an English joke")!=std::string::npos);
    for(int i=0;i<3;++i) {
      auto e=old_event;e.event_id="ordinary-"+std::to_string(i);e.text=i==2?"I am "+name:"Tell me more";
      const realtime::IdentitySessionOrigin current(epoch,continued.identity.revision,app.identity_session_sequence_);
      assert(app.memory_->IdentitySessionEvent(current,e));
    }
    assert(Reply([&](auto cb){return app.memory_->InspectTransportResume(epoch,cb);}).identity==continued.identity);
    assert(sessions==before_recovery); // No new identity action on ordinary turns or the same name.
    disconnect=true; // Server really closes the active WebSocket on the next PCM append.
    Wait([&]{return sessions==before_recovery+1 && app.session_ready_.load() && !app.memory_switching_.load();});
    assert(greetings==hello_count && app.memory_epoch_.load()==epoch && !app.identity_pending_.load());
    app.OnServerExitIntent();assert(app.gate_->step()==wakeup::Step::kActive);
    app.OnWakeInterrupt("offline repeated wake");
    Wait([&]{return sessions==before_recovery+2 && app.session_ready_.load() && !app.memory_switching_.load();});
    assert(greetings==hello_count && app.memory_epoch_.load()==epoch);
    app.OnAsrFinal("退出");assert(app.gate_->step()==wakeup::Step::kIdle);
    Wait([&]{std::lock_guard<std::mutex> l(app.control_mu_);return !app.close_requested_;});
    assert(app.trigger_->FireFromText("豆包同学"));
    Wait([&]{return sessions==before_recovery+3 && app.session_ready_.load() && !app.memory_switching_.load();});
    Wait([&]{return greetings==hello_count+1;});assert(!app.identity_pending_.load());
    const auto rewake=Reply([&](auto cb){return app.memory_->InspectTransportResume(app.memory_epoch_.load(),cb);});
    assert(rewake.identity.child_id==identified.identity.child_id && rewake.instructions.find("Tell me an English joke")!=std::string::npos);
    // A provider that never returns audio must still end on the real 30s idle timer.
    const realtime::IdentitySessionOrigin current(app.memory_epoch_.load(),rewake.identity.revision,app.identity_session_sequence_);
    const auto silent=p.Receive(J{{"type","conversation.item.input_audio_transcription.completed"},{"event_id","silent-correction"},{"item_id","silent-input"},{"transcript","Tell me a joke"}}.dump());
    app.OnAsrFinal(silent.text);assert(app.memory_->IdentitySessionEvent(current,silent));
    for(int i=0;i<3500 && app.gate_->step()!=wakeup::Step::kIdle;++i)std::this_thread::sleep_for(10ms);
    assert(app.gate_->step()==wakeup::Step::kIdle);
    Wait([&]{std::lock_guard<std::mutex> l(app.mu_);return !app.reply_pending_;});
    app.Stop();
    std::cout<<"PASS actual App ControlLoop unknown-name="<<name<<" original fork/read PCM transferred once; scripted localWS ASR original Identity; old prompt stopped, known actor session and next question accepted\n";
  }
};
}
int main(int argc,char** argv) {
  assert(argc==2);const bool privacy=std::string(argv[1])=="privacy";const bool cold_fault=std::string(argv[1])=="cold-fault";const bool interrupt_identity=std::string(argv[1])=="barge";const std::string name=(cold_fault || interrupt_identity || privacy)?"Amy":argv[1];const bool adult=name.rfind("我是",0)==0;assert(name=="Amy" || name=="Ben" || adult || name=="unclear");
  char tmp[]="/tmp/xz-identity-app-XXXXXX";assert(mkdtemp(tmp));const std::string dir=tmp;
  const std::string old=getenv("PATH")?getenv("PATH"):"";
  {std::ofstream f(dir+"/arecord");f<<"#!/usr/bin/env python3\nimport sys,time,struct\n"
    "with open('/private-capture.raw','rb') as f:\n f.seek(274560*16);raw=f.read(80160*16)\n"
    "speech=[v[0]>>8 for v in struct.iter_unpack('<iiii',raw)]\nseed=749;phase=0;index=len(speech)\n"
    "inject="<<(cold_fault?"True":"False")<<";mark='"<<dir<<"/cold-fault-once'\n"
    "try: open(mark).read();inject=False\nexcept: pass\n"
    "if inject: open(mark,'w').write('injected')\n"
    "while True:\n"
    " try: requested=int(open('"<<dir<<"/speech-phase').read())\n except: requested=0\n"
    " if requested!=phase: index=0 if requested else len(speech);phase=requested\n"
    " block=bytearray()\n for i in range(1440):\n"
    "  seed=(seed*1664525+1013904223)&0xffffffff;r=((seed>>16)&8191)-4096;m=int(.5*r)+(speech[index] if index<len(speech) else 0);index+=1\n"
    "  block+=struct.pack('<iiii',m<<16,m<<16,m<<16,r<<16)\n"
    " sys.stdout.buffer.write(block);sys.stdout.buffer.flush();time.sleep(.35 if inject else .09);inject=False\n"
    " try:\n  fault=open('"<<dir<<"/capture-fault').read();__import__('os').unlink('"<<dir<<"/capture-fault')\n"
    " except: fault=''\n"
    " if fault=='gap': time.sleep(.35)\n"
    " if fault=='overrun': sys.stderr.write('overrun!!!\\n');sys.stderr.flush()\n";}
  {std::ofstream f(dir+"/aplay");f<<"#!/usr/bin/env python3\nimport os,time\np='"<<dir<<"/written-bytes'\nwhile True:\n b=os.read(0,4096)\n if not b:\n  time.sleep(.5)\n  open('"<<dir<<"/tail-drained','w').write('done')\n  break\n time.sleep(.04)\n try: n=int(open(p).read())\n except: n=0\n with open(p+'.tmp','w') as o: o.write(str(n+len(b)))\n os.replace(p+'.tmp',p)\n";}
  assert(chmod((dir+"/arecord").c_str(),0700)==0 && chmod((dir+"/aplay").c_str(),0700)==0);assert(setenv("PATH",(dir+":"+old).c_str(),1)==0);
  const int port=ix::getFreePort();ix::WebSocketServer server(port,"127.0.0.1");
  std::mutex state_mu;std::atomic<int> sessions{0},name_sent{0},next_sent{0};std::atomic<bool> next_allowed{false},allow_name{false},disconnect{false};std::atomic<int> greetings{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState> state,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg){
    if(msg->type!=ix::WebSocketMessageType::Message)return;const auto j=J::parse(msg->str);const std::string type=j.at("type");
    static std::unordered_map<std::string,int> socket_sessions;
    std::lock_guard<std::mutex> l(state_mu);int& session=socket_sessions[state->getId()];
    if(type=="session.create") {
      session=++sessions;const auto instructions=j.dump();
      if(privacy && session>=4) {
        assert(instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos && instructions.find("BEN_PRIVATE_TEST_HISTORY")!=std::string::npos);
      }else if(session>=4) {
        assert(instructions.find("Tell me an English joke")!=std::string::npos);
        assert(instructions.find("fixture English answer")!=std::string::npos);
        assert(instructions.find("Never ask who is speaking again")!=std::string::npos);
        assert(instructions.find(name=="Ben"?"AMY_PRIVATE_TEST_HISTORY":"BEN_PRIVATE_TEST_HISTORY")==std::string::npos);
      }
      if(session==2)assert(instructions.find("Identity is unknown")!=std::string::npos && instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos && instructions.find("BEN_PRIVATE_TEST_HISTORY")==std::string::npos);
      if(session==3) {
        if(name=="Amy")assert(instructions.find("AMY_PRIVATE_TEST_HISTORY")!=std::string::npos && instructions.find("BEN_PRIVATE_TEST_HISTORY")==std::string::npos);
        if(name=="Ben")assert(instructions.find("BEN_PRIVATE_TEST_HISTORY")!=std::string::npos && instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos);
        if(name=="unclear")assert(instructions.find("Never ask who is speaking again")!=std::string::npos && instructions.find("BEN_PRIVATE_TEST_HISTORY")==std::string::npos && instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos);
        if(adult)assert(instructions.find("BEN_PRIVATE_TEST_HISTORY")==std::string::npos && instructions.find("AMY_PRIVATE_TEST_HISTORY")==std::string::npos);
      }
      ws.sendText(J{{"type","session.created"},{"session",{{"id","loopback-"+std::to_string(session)}}}}.dump());
    }else if(type=="input_audio_buffer.append" && disconnect.exchange(false)) {ws.close();
    }else if(type=="speech_text_buffer.commit") {
      ++greetings;
      ws.sendText(R"({"type":"response.output_audio.delta","event_id":"g-a","question_id":"g-q","response_id":"g-r","delta":"AQI="})");
      if(session>1) {ws.sendText(R"({"type":"response.output_audio.done","event_id":"g-d","question_id":"g-q","response_id":"g-r"})");ws.sendText(R"({"type":"response.done","response_id":"g-r","usage":{"total_tokens":1}})");}
    }else if(type=="input_audio_buffer.append" && session==2 && allow_name && name_sent.exchange(1)==0) {
      // A scripted provider transcript, distinct from the historical PCM source.
      ws.sendText(J{{"type","conversation.item.input_audio_transcription.completed"},{"event_id","name-asr"},{"item_id","name-input"},{"transcript",name}}.dump());
      ws.sendText(J{{"type","response.output_audio.delta"},{"event_id","name-a"},{"question_id","name-input"},{"response_id","name-r"},{"delta",std::string(128000,'A')}}.dump());
    }else if(type=="input_audio_buffer.append" && session==3 && next_allowed && next_sent.exchange(1)==0) {
      ws.sendText(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"next-asr","item_id":"next-input","transcript":"Tell me an English joke"})");
      ws.sendText(R"({"type":"response.output_audio.delta","event_id":"next-a","question_id":"next-q","response_id":"next-r","delta":"AwQ="})");
      ws.sendText(R"({"type":"response.output_text.done","event_id":"next-text","question_id":"next-q","response_id":"next-r","text":"fixture English answer"})");
      ws.sendText(R"({"type":"response.output_audio.done","event_id":"next-d","question_id":"next-q","response_id":"next-r"})");
      ws.sendText(R"({"type":"response.done","response_id":"next-r","usage":{"total_tokens":1}})");
    }
  });assert(server.listenAndStart());app::AppOfflineProbe::Run(dir,port,name,sessions,name_sent,allow_name,next_allowed,next_sent,disconnect,greetings,interrupt_identity,privacy);
  server.stop();assert(setenv("PATH",old.c_str(),1)==0);std::filesystem::remove_all(dir);
  std::cout<<"Fixture only: actual captured historical human word is not these names, scripted ASR is not cloud recognition or child/room acceptance\n";
}
