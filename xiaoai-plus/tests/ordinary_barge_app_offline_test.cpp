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
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include "app/app.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
using J=memory::Json;
template<class F> void Wait(F f) {
  for(int i=0;i<800 && !f();++i)std::this_thread::sleep_for(10ms);assert(f());
}
template<class F> memory::ResumeResult Reply(F f) {
  std::promise<memory::ResumeResult> p;auto future=p.get_future();
  assert(f([&](auto r){p.set_value(std::move(r));}));
  assert(future.wait_for(3s)==std::future_status::ready);return future.get();
}
namespace xiaoai_plus::app {
// Drives the real App owner and capture/Player/Service/Client implementations.
// A forked fixture producer mixes the authorised historical human sample with
// known synthetic reference in memory; it is not household voice acceptance.
struct AppOfflineProbe {
  static void Run(const std::string& dir,int port,std::atomic<int>& append,
      std::atomic<int>& mute,std::atomic<int>& commits) {
    config::Config cfg;cfg.realtime.protocol="v3";cfg.realtime.api_key="offline-loopback-only";
    cfg.realtime.preset.ws_url="ws://127.0.0.1:"+std::to_string(port);
    cfg.conversation.usage_dir=dir+"/usage";cfg.memory.enabled=true;cfg.memory.cache_dir=dir+"/memory";
    cfg.conversation.ordinary_barge_in=true;
    App app(cfg,false,false,false,cfg.wakeup.s32_shift,cfg.wakeup.input_channel);
    app.kws_listener_->Close(); // Real KWS model loads, recognition is outside this probe.
    std::atomic<int> actions{0};
    auto start_memory=[&] {
      app.memory_=std::make_unique<memory::Service>(cfg.memory,
        [](const std::string& path,const J&)->memory::Reply {
          if(path=="/device/snapshot")return {200,{{"generation",9},{"expires_at",std::time(nullptr)+3600},
            {"children",J::array({J{{"id","amy"},{"version",4},{"profile",{{"name","Amy"},{"aliases",J::array({"Amy"})}}},{"summaries",J::array()}}})}}};
          return {200,J::object()};
        },[&](memory::Action){++actions;});
      app.memory_->Start();Wait([&]{return app.memory_->cache_revision()>0;});
    };
    start_memory();
    assert(app.memory_->Begin(42));Wait([&]{return actions==1;});
    realtime::duplex::Session parser;parser.Create("fixture","fixture");
    parser.Receive(R"({"type":"session.created","session":{"id":"parser"}})");
    assert(app.memory_->Event(42,parser.Receive(R"({"type":"conversation.item.input_audio_transcription.completed","event_id":"identify","item_id":"identify","transcript":"I am Amy"})")));
    Wait([&]{return actions==2;});assert(app.memory_->Activate(42));
    const auto identity=Reply([&](auto cb){return app.memory_->InspectTransportResume(42,cb);});
    assert(identity.approved && identity.identity.child_id=="amy");
    app.memory_epoch_=42;app.running_=true;assert(app.player_->Start());
    assert(app.client_->Start());assert(app.client_->StartSession(2s,"fixture legacy"));
    app.gate_->Activate("offline_owner_fixture");app.identity_pending_=false;
    assert(app.StartRecorder());
    Wait([&]{std::lock_guard<std::mutex> lock(app.barge_mu_);return app.capture_last_generation_==1;});
    std::this_thread::sleep_for(500ms);app.OnSetAiSpeaking(true);
    std::this_thread::sleep_for(600ms); // Establish the synthetic echo path before speech.
    int speech_phase=0;
    auto request=[&] {
      {std::ofstream start(dir+"/speech-phase");start<<++speech_phase;}
      Wait([&]{std::lock_guard<std::mutex> lock(app.barge_mu_);return app.barge_collecting_;});
      app.HandleOrdinaryBarge();
      std::lock_guard<std::mutex> lock(app.barge_mu_);
      assert(app.fresh_granted_ && app.fresh_envelope_.valid());return app.fresh_envelope_;
    };
    const auto first=request();Wait([&]{return append.load()>0;});
    // Capture runs on a real forked producer while old socket is retired and
    // a real fresh session.created receipt grants a new immutable envelope.
    assert(first.epoch()==42 && first.capture()==app.recorder_->Generation());
    {std::ofstream quiet(dir+"/speech-phase");quiet<<0;}
    std::this_thread::sleep_for(300ms); // The simulated user finishes before the answer.
    app.fresh_client_->HandleTextBound(first,R"({"type":"response.output_audio.delta","event_id":"audio1","question_id":"q1","response_id":"r1","delta":"AQI="})");
    Wait([&]{return mute.load()>=2;});
    app.fresh_client_->HandleTextBound(first,R"({"type":"response.done","usage":{"total_tokens":1}})");
    {std::lock_guard<std::mutex> lock(app.barge_mu_);assert(!app.fresh_done_ && !app.fresh_audio_done_);}
    // A legal tail after response.done is still played by the same generation.
    app.fresh_client_->HandleTextBound(first,R"({"type":"response.output_audio.delta","event_id":"tail1","question_id":"q1","response_id":"r1","delta":"AwQ="})");
    app.fresh_client_->HandleTextBound(first,R"({"type":"response.output_audio.done","event_id":"done1","question_id":"q1","response_id":"r1"})");
    Wait([&]{std::lock_guard<std::mutex> lock(app.barge_mu_);return app.fresh_done_;});
    const auto same=Reply([&](auto cb){return app.memory_->InspectTransportResume(42,cb);});
    assert(same.approved && same.identity==identity.identity && actions==2);
    {std::ofstream quiet(dir+"/speech-phase");quiet<<0;}
    std::this_thread::sleep_for(600ms);
    {std::ofstream fault(dir+"/capture-fault");fault<<"overrun";}
    Wait([&]{return !app.capture_healthy_.load();});
    app.RestartBargeCapture();
    Wait([&]{return app.capture_healthy_.load();});
    assert(app.recorder_->Generation()==first.capture()+1 && app.gate_->step()==wakeup::Step::kActive);
    std::this_thread::sleep_for(600ms);
    const int append_before=append;const auto second=request();
    assert(second.transport()>first.transport() && second.input()>first.input());
    assert(second.identity()==first.identity() && second.epoch()==42 && actions==2);
    Wait([&]{return append.load()>append_before;});
    app.fresh_client_->HandleTextBound(first,R"({"type":"response.output_audio.delta","event_id":"retired","question_id":"q1","response_id":"r1","delta":"AQI="})");
    {std::lock_guard<std::mutex> lock(app.barge_mu_);assert(app.fresh_granted_ && !app.fresh_audio_done_);}
    // The actual 800ms residual-VAD silence endpoint follows the historical
    // human window; it is not an ASR/send ACK or a manufactured voice flag.
    Wait([&]{return commits.load()==1;});
    app.OnNativeWake();assert(app.gate_->step()==wakeup::Step::kIdle);
    assert(!app.fresh_client_->EnqueueFreshAudio(second,std::vector<uint8_t>(640,0)));
    assert(app.fresh_client_->RetireFreshTransport(second,[&]{return app.player_->Interrupt(app.player_->Generation());}));
    // An unconfirmed speaker is a separate fixture: a recent Amy re-wake
    // deliberately retains her identity for ten minutes in the same Service.
    app.memory_->Stop();cfg.memory.cache_dir=dir+"/unknown-memory";start_memory();
    assert(app.memory_->Begin(84));Wait([&]{return actions==3;});
    app.memory_epoch_=84;
    {std::lock_guard<std::mutex> lock(app.barge_mu_);app.fresh_envelope_={};app.fresh_mode_=false;app.capture_context_={};}
    app.legacy_effects_live_=true;
    assert(app.client_->Start());assert(app.client_->StartSession(2s,"fixture pending identity"));
    app.gate_->Activate("offline_unconfirmed_identity_fixture");
    {std::ofstream quiet(dir+"/speech-phase");quiet<<0;}
    app.OnSetAiSpeaking(true);std::this_thread::sleep_for(600ms);
    {std::ofstream speak(dir+"/speech-phase");speak<<3;}
    Wait([&]{std::lock_guard<std::mutex> lock(app.barge_mu_);return app.barge_collecting_;});
    app.HandleOrdinaryBarge();assert(app.gate_->step()==wakeup::Step::kActive);
    {std::lock_guard<std::mutex> lock(app.control_mu_);
      assert(!app.rephrase_requested_);}
    const auto unknown=Reply([&](auto cb){return app.memory_->InspectTransportResume(84,cb);});
    assert(unknown.approved && unknown.identity.kind=="unknown" && unknown.identity.child_id.empty());
    {std::lock_guard<std::mutex> lock(app.barge_mu_);assert(app.fresh_granted_ && app.fresh_envelope_.epoch()==84 && app.fresh_envelope_.identity()==unknown.identity.revision);}
    app.Stop();assert(app.recorder_->Generation()==2);
    std::cout<<"PASS real App captured-source owner/fresh-WebSocket/mute/800ms endpoint/audio-tail/current Player write-drain/new-input/same-identity/native-exit wiring only\n";
  }
};
}
int main() {
  char temp[]="/tmp/xz-app-owner-XXXXXX";assert(mkdtemp(temp));const std::string dir=temp;
  const std::string old=getenv("PATH")?getenv("PATH"):"";
  {std::ofstream s(dir+"/arecord");s<<"#!/usr/bin/env python3\nimport sys,time,struct,os\n"
    "with open('/private-capture.raw','rb') as f:\n f.seek(274560*16);original=f.read(80160*16)\n"
    "speech=[v[0]>>8 for v in struct.iter_unpack('<iiii',original)]\n"
    "seed=749;phase=0;index=len(speech)\n"
    "while True:\n"
    " try: requested=int(open('"<<dir<<"/speech-phase').read())\n"
    " except: requested=0\n"
    " if requested!=phase: index=0 if requested else len(speech);phase=requested\n"
    " block=bytearray()\n"
    " for i in range(1440):\n"
    "  seed=(seed*1664525+1013904223)&0xffffffff;r=((seed>>16)&8191)-4096\n"
    "  m=int(.5*r)+(speech[index] if index<len(speech) else 0);index+=1\n"
    "  block+=struct.pack('<iiii',m<<16,m<<16,m<<16,r<<16)\n"
    " sys.stdout.buffer.write(block);sys.stdout.buffer.flush();time.sleep(.09)\n"
    " if os.path.exists('"<<dir<<"/capture-fault'):\n  os.unlink('"<<dir<<"/capture-fault');sys.stderr.write('overrun!!!\\n');sys.stderr.flush()\n";}
  {std::ofstream s(dir+"/aplay");s<<"#!/bin/sh\nexec cat >/dev/null\n";}
  assert(chmod((dir+"/arecord").c_str(),0700)==0 && chmod((dir+"/aplay").c_str(),0700)==0);
  assert(setenv("PATH",(dir+":"+old).c_str(),1)==0);
  const int port=ix::getFreePort();ix::WebSocketServer server(port,"127.0.0.1");
  std::atomic<int> sessions{0},append{0},mute{0},commits{0};
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>,ix::WebSocket& ws,const ix::WebSocketMessagePtr& msg) {
    if(msg->type!=ix::WebSocketMessageType::Message)return;const auto j=J::parse(msg->str);const auto type=j.at("type");
    if(type=="session.create")ws.sendText(J{{"type","session.created"},{"session",{{"id","offline-"+std::to_string(++sessions)}}}}.dump());
    if(type=="input_audio_buffer.append")++append;
    if(type=="input_audio_mute.commit")++mute;
    if(type=="input_audio_buffer.commit")++commits;
  });assert(server.listenAndStart());
  app::AppOfflineProbe::Run(dir,port,append,mute,commits);server.stop();
  assert(sessions==5);assert(setenv("PATH",old.c_str(),1)==0);std::filesystem::remove_all(dir);
  std::cout<<"PASS five actual sessions; unknown identity stays in its own input without borrowing Amy\n"
    <<"Fixture API/PCM only, no provider TLS/physical microphone/speaker/native-feedback/child/room acceptance\n";
}
