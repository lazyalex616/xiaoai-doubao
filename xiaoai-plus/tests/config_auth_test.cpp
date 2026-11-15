#ifdef NDEBUG
#error requires active assertions
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "config/config.hpp"

using xiaoai_plus::config::Config;

int main() {
  Config current;
  current.realtime.api_key = "test-only-key";
  current.validate();
  assert(current.audio.playback_gain == 0.90f);
  for (float gain : {0.0f, -0.1f, 1.01f}) {
    auto bad = current; bad.audio.playback_gain = gain; bool rejected = false;
    try { bad.validate(); } catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
  }
  assert(current.stop_key.device.empty() && current.stop_key.key_code == -1);
  Config key = current;
  key.stop_key.device = "/dev/input/event0"; key.stop_key.key_code = 164; key.validate();
  for (const std::string node : {"/dev/input/event", "/tmp/test", "/dev/input/event0/other"}) {
    key.stop_key.device = node; bool bad = false;
    try { key.validate(); } catch (const std::runtime_error&) { bad = true; }
    assert(bad);
  }
  key.stop_key.device.clear();
  for (int code : {0, -2, 768}) {
    key.stop_key.key_code = code; bool bad = false;
    try { key.validate(); } catch (const std::runtime_error&) { bad = true; }
    assert(bad);
  }
  const auto current_headers = current.realtime.request_headers();
  assert(current_headers.at("X-Api-Key") == "test-only-key");
  assert(current_headers.size() == 1);
  assert(current_headers.count("X-Api-App-ID") == 0);
  assert(current_headers.count("X-Api-Access-Key") == 0);
  assert(current_headers.count("X-Api-App-Key") == 0);
  assert(current.realtime.preset.ca_file != "NONE");
  assert(current.realtime.preset.model == "1.2.6.1");
  assert(current.realtime.preset.ws_url ==
         "wss://openspeech.bytedance.com/api/v3/duplex/realtime/dialogue");
  assert(current.conversation.idle_seconds == 30);
  assert(current.conversation.absolute_timeout_seconds == 0);
  assert(current.conversation.duplex_mode == "half");
  assert(current.conversation.daily_token_limit == 0);
  assert(current.wakeup.kws_agc);
  assert(current.wakeup.s32_shift == 16);
  assert(current.wakeup.input_channel == -1);
  assert(current.wakeup.kws_threshold == 0.10f);
  assert(current.wakeup.kws_num_threads==2);
  for(int threads:{0,-1}) {
    auto bad=current;bad.wakeup.kws_num_threads=threads;bool rejected=false;
    try{bad.validate();}catch(const std::runtime_error&){rejected=true;}assert(rejected);
  }
  assert(current.wakeup.exit_playback_threshold==0.45f);
  for(float threshold:{0.29f,1.01f}) {
    auto bad=current;bad.wakeup.exit_playback_threshold=threshold;bool rejected=false;
    try{bad.validate();}catch(const std::runtime_error&){rejected=true;}assert(rejected);
  }
  Config full = current;
  full.conversation.duplex_mode = "full";
  full.validate();
  Config bad_duplex = current;
  bad_duplex.conversation.duplex_mode = "invalid";
  bool duplex_rejected = false;
  try { bad_duplex.validate(); } catch (const std::runtime_error&) { duplex_rejected = true; }
  assert(duplex_rejected);
  Config bad_limit = current;
  bad_limit.conversation.daily_token_limit = 1000000001;
  bool limit_rejected = false;
  try { bad_limit.validate(); } catch (const std::runtime_error&) { limit_rejected = true; }
  assert(limit_rejected);
  for (int allowed : {15, 30, 60, 600, 900}) {
    Config option = current;
    option.conversation.idle_seconds = allowed;
    option.validate();
  }
  Config invalid_idle = current;
  invalid_idle.conversation.idle_seconds = 901;
  bool idle_rejected = false;
  try { invalid_idle.validate(); } catch (const std::runtime_error&) { idle_rejected = true; }
  assert(idle_rejected);
  Config timed = current;
  timed.conversation.absolute_timeout_seconds = 180;
  timed.validate();
  timed.conversation.absolute_timeout_seconds = 86401;
  bool absolute_rejected = false;
  try { timed.validate(); } catch (const std::runtime_error&) { absolute_rejected = true; }
  assert(absolute_rejected);

  Config obsolete_model = current;
  obsolete_model.realtime.preset.model = "1.2.1.1";
  bool rejected = false;
  try {
    obsolete_model.validate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  Config missing_ca = current;
  missing_ca.realtime.preset.ca_file = "NONE";
  rejected = false;
  try {
    missing_ca.validate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  Config legacy;
  legacy.realtime.app_id = "test-only-app";
  legacy.realtime.access_token = "test-only-token";
  rejected = false;
  try { legacy.validate(); } catch (const std::runtime_error&) { rejected = true; }
  assert(rejected);

  Config mixed = current;
  mixed.realtime.access_token = "test-only-token";
  rejected = false;
  try {
    mixed.validate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  Config placeholder;
  placeholder.realtime.api_key = "your_api_key";
  rejected = false;
  try {
    placeholder.validate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  Config incomplete;
  incomplete.realtime.api_key.clear();
  rejected = false;
  try {
    incomplete.validate();
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);

  char filename[] = "/tmp/xiaoai-plus-config-auth-test-XXXXXX";
  const int fd = mkstemp(filename);
  assert(fd >= 0);
  close(fd);
  const std::string path = filename;
  {
    std::ofstream out(path);
    out << "[realtime]\napi_key = test-only-key\n"
           "[wakeup]\nkeywords_file = /tmp/test-only-keywords.txt\n"
           "kws_agc = off\ns32_shift = 8\ninput_channel = 2\nkws_threshold = 0.25\nkws_num_threads = 1\n"
           "[conversation]\nidle_seconds = 600\nabsolute_timeout_seconds = 0\nduplex_mode = full\ndaily_token_limit = 123\n";
  }
  assert(chmod(path.c_str(), 0600) == 0);
  const auto loaded = xiaoai_plus::config::load(path);
  for(const std::string mode:{"v3","o2"}) {
    {std::ofstream out(path);out<<"[realtime]\napi_key=test-only-key\nprotocol="<<mode<<"\n[realtime.o2]\nend_smooth_window_ms=3000\n";}
    const auto option=xiaoai_plus::config::load(path);
    assert(option.realtime.protocol==mode && option.realtime.o2_end_smooth_window_ms==3000);
  }
  assert(loaded.realtime.protocol=="o2" && loaded.realtime.o2_end_smooth_window_ms==2500);
  assert(loaded.realtime.ws_url()=="wss://openspeech.bytedance.com/api/v3/realtime/dialogue");
  assert(loaded.realtime.request_headers().at("X-Api-Key") == "test-only-key");
  assert(loaded.realtime.request_headers().at("X-Api-Resource-Id")=="volc.speech.dialog");
  assert(loaded.realtime.request_headers().size()==3);
  assert(loaded.wakeup.keywords_file == "/tmp/test-only-keywords.txt");
  assert(!loaded.wakeup.kws_agc);
  assert(loaded.wakeup.s32_shift == 8);
  assert(loaded.wakeup.input_channel == 2);
  assert(loaded.wakeup.kws_threshold == 0.25f);
  assert(loaded.wakeup.kws_num_threads==1);
  assert(loaded.conversation.idle_seconds == 600);
  assert(loaded.conversation.absolute_timeout_seconds == 0);
  assert(loaded.conversation.duplex_mode == "full");
  assert(loaded.conversation.daily_token_limit == 123);
  for(const std::string value:{"0","-1","2tail","999999999999999999999"}) {
    {std::ofstream out(path);out<<"[realtime]\napi_key = test-only-key\n[wakeup]\nkws_num_threads = "<<value<<"\n";}
    bool bad=false;try{(void)xiaoai_plus::config::load(path);}catch(const std::runtime_error&){bad=true;}assert(bad);
  }
  for (const std::string value : {"on", "off", "true", "invalid"}) {
    { std::ofstream out(path); out << "[realtime]\napi_key = test-only-key\n[wakeup]\nkws_agc = " << value << "\n"; }
    bool bad = false;
    try { auto cfg = xiaoai_plus::config::load(path); assert(cfg.wakeup.kws_agc == (value == "on")); }
    catch (const std::runtime_error&) { bad = true; }
    assert(bad == (value != "on" && value != "off"));
  }
  { std::ofstream out(path); out << "[realtime]\napi_key = test-only-key\n[stop_key]\ndevice = /dev/input/event1\nkey_code = 164\n[audio]\nplayback_gain = 0.75\n"; }
  const auto mapped = xiaoai_plus::config::load(path);
  assert(mapped.stop_key.device == "/dev/input/event1" && mapped.stop_key.key_code == 164);
  assert(mapped.audio.playback_gain == 0.75f);
  for (const std::string raw : {"164tail", "-2", "999999999999999999999"}) {
    { std::ofstream out(path); out << "[realtime]\napi_key = test-only-key\n[stop_key]\nkey_code = " << raw << "\n"; }
    bool bad = false; try { (void)xiaoai_plus::config::load(path); }
    catch (const std::runtime_error&) { bad = true; } assert(bad);
  }
  const auto secret_path=path+"-secret";
  auto secret_config=[&](const std::string& target) {
    std::ofstream out(path);
    out << "[realtime]\napi_key = test-only-key\n[memory]\nenabled = on\nserver = https://example.invalid\n"
        << "device_id = test-device\ndevice_secret = " << std::string(64,'b')
        << "\ndevice_secret_file = " << target << "\n";
  };
  auto write_secret=[&](const std::string& text) {
    std::ofstream out(secret_path);out<<text;out.close();assert(chmod(secret_path.c_str(),0600)==0);
  };
  write_secret(std::string(64,'a'));secret_config(secret_path);
  auto memory_cfg=xiaoai_plus::config::load(path);
  assert(memory_cfg.memory.enabled && memory_cfg.memory.device_secret==std::string(64,'a'));
  assert(memory_cfg.memory.device_id=="test-device");
  assert(memory_cfg.memory.identity_text_log);
  {std::ofstream out(path,std::ios::app);out<<"identity_text_log = off\n";}
  assert(!xiaoai_plus::config::load(path).memory.identity_text_log);
  secret_config(secret_path);
  write_secret(std::string(64,'a')+"\n");assert(xiaoai_plus::config::load(path).memory.enabled);
  for(const auto& text:{std::string(63,'a'),std::string(65,'a'),std::string(64,'g'),std::string(64,'a')+"\n\n"}) {
    write_secret(text);memory_cfg=xiaoai_plus::config::load(path);
    assert(!memory_cfg.memory.enabled && memory_cfg.memory.secret_file_failed && memory_cfg.memory.device_secret.empty());
    assert(memory_cfg.realtime.api_key=="test-only-key");
  }
  write_secret(std::string(64,'a'));assert(chmod(secret_path.c_str(),0640)==0);
  assert(!xiaoai_plus::config::load(path).memory.enabled);
  assert(chmod(secret_path.c_str(),0400)==0);assert(!xiaoai_plus::config::load(path).memory.enabled);
  const auto link=secret_path+"-link";assert(symlink(secret_path.c_str(),link.c_str())==0);
  secret_config(link);assert(!xiaoai_plus::config::load(path).memory.enabled);unlink(link.c_str());
  secret_config(secret_path+"-absent");assert(!xiaoai_plus::config::load(path).memory.enabled);
  secret_config("/tmp");assert(!xiaoai_plus::config::load(path).memory.enabled);unlink(secret_path.c_str());
  {std::ofstream out(path);out<<"[realtime]\napi_key = test-only-key\n[memory]\nenabled = on\nserver_url = https://example.invalid\ndevice_secret = "<<std::string(64,'b')<<"\n";}
  assert(xiaoai_plus::config::load(path).memory.enabled); // Inline compatibility.
  {std::ofstream out(path);out<<"[memory]\nenabled = on\nserver = https://example.invalid\ndevice_secret = "<<std::string(64,'b')<<"\n";}
  assert(xiaoai_plus::config::load(path,true).memory.enabled);
  rejected=false;try{(void)xiaoai_plus::config::load(path);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
  assert(chmod(path.c_str(), 0644) == 0);
  rejected = false;
  try {
    (void)xiaoai_plus::config::load(path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
  assert(chmod(path.c_str(), 0600) == 0);
  const std::string symlink_path = path + "-symlink";
  assert(symlink(path.c_str(), symlink_path.c_str()) == 0);
  rejected = false;
  try {
    (void)xiaoai_plus::config::load(symlink_path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
  std::remove(symlink_path.c_str());

  char dirname[] = "/tmp/xiaoai-plus-config-dir-XXXXXX";
  assert(mkdtemp(dirname) != nullptr);
  const std::string bad_parent_path = std::string(dirname) + "/config.ini";
  {
    std::ofstream out(bad_parent_path);
    out << "[realtime]\napi_key = test-only-key\n";
  }
  assert(chmod(bad_parent_path.c_str(), 0600) == 0);
  assert(chmod(dirname, 0777) == 0);
  rejected = false;
  try {
    (void)xiaoai_plus::config::load(bad_parent_path);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  assert(rejected);
  std::remove(bad_parent_path.c_str());
  rmdir(dirname);
  for(const auto& value : {std::string("smart_turn = yes"),std::string("smart_turn_complete_ms = 299"),
      std::string("smart_turn_open_ms = 20001"),std::string("smart_turn_open_ms = -1")}) {
    {std::ofstream out(path);out<<"[realtime]\napi_key = test-only-key\n[conversation]\n"<<value<<"\n";}
    rejected=false;try{(void)xiaoai_plus::config::load(path);}catch(...){rejected=true;}assert(rejected);
  }
  {std::ofstream out(path);out<<"[realtime]\napi_key = test-only-key\n[conversation]\nsmart_turn = on\nsmart_turn_complete_ms = 300\nsmart_turn_open_ms = 20000\n";}
  auto smart=xiaoai_plus::config::load(path).conversation;
  assert(smart.smart_turn && smart.smart_turn_complete_ms==300 && smart.smart_turn_open_ms==20000);
  std::remove(path.c_str());
}
