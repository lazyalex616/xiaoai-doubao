#ifdef NDEBUG
#error requires assertions
#endif
#include <cassert>
#include <iostream>
#include "realtime/o2_protocol.hpp"
#include "realtime/duplex_protocol.hpp"
using namespace xiaoai_plus::realtime;
using J=nlohmann::json;
using namespace protocol;
int main() {
  o2::Session wire;wire.Reset();xiaoai_plus::config::Realtime cfg;
  auto encode=[&](const std::string& s){auto b=wire.Encode(s,cfg);return DecodeFrame({b.begin(),b.end()});};
  auto c=encode(R"({"type":"connection.start"})");assert(c.event_id==EventId::kStartConnection);
  duplex::Session session;
  c=encode(session.Create("same identity/coach/R6 instructions","voice",true));
  const auto sid=*c.session_id;auto p=c.JsonPayload();
  assert(p["dialog"]["system_role"]=="same identity/coach/R6 instructions");
  const auto style=p["dialog"]["speaking_style"].get<std::string>();
  assert(style.find("Usually 2 short sentences, never more than 3")!=std::string::npos);
  assert(style.find("a fun sound like \"Roar!\" doesn't count.")!=std::string::npos);
  assert(style.find("At most one question, only at the end.")!=std::string::npos);
  assert(style.find("No third sentence")==std::string::npos && style.find("不列选项")==std::string::npos);
  assert(p["asr"]["extra"]["end_smooth_window_ms"]==2500);
  assert(p["asr"]["extra"]["enable_custom_vad"]==true);
  assert(p["dialog"]["extra"]["strict_audit"]==true);
  assert(p["dialog"]["extra"]["model"]=="1.2.1.1");
  assert(p["tts"]["speaker"]==cfg.o2_voice);
  auto receive=[&](int id,J body=J::object()) {
    auto b=BuildJsonFrame(MessageType::kFullServerResponse,static_cast<EventId>(id),body,sid);
    return wire.Receive({b.begin(),b.end()});
  };
  auto e=receive(150);assert(session.Receive(e.at(0)).type=="session.created");
  c=encode(session.Greeting("Is that Amy or Ben?"));assert(c.event_id==EventId::kSayHello);
  assert(c.JsonPayload()["content"]=="Is that Amy or Ben?");
  receive(450,{{"question_id","q1"}});
  e=receive(451,{{"results",J::array({{{"text","我是 Amy"},{"is_interim",false}}})}});
  assert(e.size()==1 && session.Receive(e[0]).type=="conversation.item.input_audio_transcription.delta");
  assert(session.Receive(e[0]).text=="我是 Amy");
  c=encode(R"({"type":"input_audio_buffer.commit"})");
  assert(c.event_id==EventId::kEndAsr && c.session_id==sid && c.JsonPayload()==J::object());
  e=receive(459);assert(e.size()==1);auto asr=session.Receive(e.at(0));
  assert(asr.type=="conversation.item.input_audio_transcription.completed" && asr.text=="我是 Amy" && asr.item_id=="q1");
  e=receive(459);assert(e.size()==1);
  assert(session.Receive(e.at(0)).type=="conversation.item.input_audio_transcription.ended_empty");
  receive(550,{{"question_id","q1"},{"reply_id","r1"},{"content","Hi "}});
  receive(550,{{"question_id","q1"},{"reply_id","r1"},{"content","Amy"}});
  e=receive(559,{{"question_id","q1"},{"reply_id","r1"}});
  assert(session.Receive(e.at(0)).text=="Hi Amy");
  auto a=wire.Audio(std::vector<uint8_t>(640,3));auto f=DecodeFrame({a.begin(),a.end()});
  assert(f.payload==std::vector<uint8_t>(640,3) && f.event_id==EventId::kTaskRequest);
  auto audio=BuildFrame(MessageType::kAudioOnlyResponse,Serialization::kRaw,EventId::kTtsResponse,
                       {1,2,3,4},sid,Compression::kNone);
  auto audio_event=[&]{return wire.Receive({audio.begin(),audio.end()});};
  assert(session.Receive(audio_event().at(0)).audio==std::vector<uint8_t>({1,2,3,4}));
  assert(wire.Encode(session.CancelResponse(),cfg).empty());assert(audio_event().empty());
  receive(350,{{"question_id","q1"},{"reply_id","r1"}});assert(audio_event().empty());
  receive(450,{{"question_id","q2"}});assert(audio_event().empty());
  receive(350,{{"question_id","q1"},{"reply_id","r1"}});assert(audio_event().empty());
  receive(350,{{"question_id","q2"},{"reply_id","r2"}});assert(audio_event().size()==1);
  receive(350,{{"question_id","q1"},{"reply_id","r1"}});assert(audio_event().empty());
  e=receive(154,{{"usage",{{"input_text_tokens",10},{"input_audio_tokens",20},
      {"cached_text_tokens",3},{"cached_audio_tokens",4},{"output_text_tokens",5},{"output_audio_tokens",6}}}});
  auto usage=session.Receive(e.at(0));assert(usage.total_tokens==48 && usage.estimated_yuan);
  assert(*usage.estimated_yuan>0);
  e=receive(359,{{"question_id","q2"},{"reply_id","r2"},{"status_code","20000002"}});
  assert(session.Receive(e.at(0)).exit_intent);
  c=encode(session.Close());assert(c.event_id==EventId::kFinishSession);
  e=receive(152);assert(session.Receive(e.at(0)).type=="session.closed");
  const std::string quota="quota exceeded for types: tokens_lifetime";
  const auto payload=J{{"error",quota}}.dump();
  std::vector<uint8_t> rejected={0x11,0xf0,0x10,0x00};
  auto u32=[&](uint32_t n){for(int shift=24;shift>=0;shift-=8)rejected.push_back(n>>shift);};
  u32(45000292);u32(payload.size());rejected.insert(rejected.end(),payload.begin(),payload.end());
  e=wire.Receive({rejected.begin(),rejected.end()});assert(e.size()==1);
  assert(J::parse(e[0])["message"]==quota);
  const auto error=session.Receive(e[0]);
  assert(error.type=="error" && error.error_code==45000292 && error.text==quota);
  bool failed=false;try{wire.Receive("bad");}catch(...){failed=true;}assert(failed);
  std::cout<<"O2 codec/event mapping, identity text, usage and canceled residual fence PASS\n";
}
