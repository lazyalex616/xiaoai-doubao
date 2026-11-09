#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <unistd.h>
#include "app/app.hpp"
using namespace xiaoai_plus;
using namespace std::chrono_literals;
namespace xiaoai_plus::app {
struct AppOfflineProbe {
  static void Cues(const std::string& pcm) {
    config::Config cfg;cfg.realtime.api_key="offline-only";App app(cfg);
    assert(app.player_->Start());assert(app.gate_->TryWakeup("fixture"));
    assert(app.cue_requested_.load()==audio::SessionCue::kWake);
    assert(!app.gate_->TryWakeup("fixture")); // Repeated wake during listening is inert.
    app.PlaySessionCue();assert(std::filesystem::file_size(pcm)==7680);
    const auto wake_generation=app.player_->Generation();
    app.PlaySessionCue();assert(app.player_->Generation()==wake_generation);
    assert(std::filesystem::file_size(pcm)==7680);
    app.OnSetAiSpeaking(true); // O2 cancel can retire Client speech before App gets a done callback.
    app.EndLocalConversation("local_kws_exit");
    assert(app.cue_requested_.load()==audio::SessionCue::kExit);
    app.PlaySessionCue();assert(std::filesystem::file_size(pcm)==15360);
    const auto exit_generation=app.player_->Generation();
    app.EndLocalConversation("local_kws_exit");app.PlaySessionCue();
    assert(app.player_->Generation()==exit_generation && std::filesystem::file_size(pcm)==15360);
    assert(app.gate_->TryWakeup("fixture"));
    assert(!app.is_ai_speaking_); // A fresh half-duplex conversation must accept mic audio.
  }
  static void Run(const std::string& fifo) {
    config::Config cfg;
    cfg.realtime.api_key = "offline-only";
    App app(cfg);
    assert(!cfg.conversation.ordinary_barge_in);
    std::atomic<int> callbacks{0};
    input::StopKey unmapped("", 164, [&] { ++callbacks; });
    input::StopKey unknown(fifo, -1, [&] { ++callbacks; });
    assert(unmapped.Start() && unknown.Start()); // No input opened or event acted on.
    assert(mkfifo(fifo.c_str(), 0600) == 0);
    const int writer = open(fifo.c_str(), O_RDWR | O_NONBLOCK); assert(writer >= 0);
    input::StopKey listener(fifo, 164, [&] { app.OnStopKey(); ++callbacks; });
    assert(listener.Start());
    auto event = [&](int code, int value) {
      input_event e{}; e.type = EV_KEY; e.code = code; e.value = value;
      assert(write(writer, &e, sizeof(e)) == sizeof(e));
    };
    auto wait = [&](int n) { for (int i=0; i<200 && callbacks<n; ++i) usleep(1000); assert(callbacks==n); };
    event(164,1); wait(1); // Idle is completely inert inside App.
    assert(!app.close_requested_ && !app.interrupt_requested_);
    assert(app.gate_->step()==wakeup::Step::kIdle);
    event(164,0); std::this_thread::sleep_for(320ms);
    const auto generation=app.player_->Generation();
    app.playback_thread_=std::thread([&] { app.PlaybackControlLoop(); });
    assert(app.gate_->CommitWakeup("fixture")); app.session_ready_=true;
    app.gate_->SetAiSpeaking(true);
    event(163,1); event(164,2); // Wrong key and repeat cannot exit.
    usleep(30000); assert(callbacks==1 && app.gate_->step()==wakeup::Step::kActive);
    event(164,1); wait(2);
    assert(app.gate_->step()==wakeup::Step::kIdle && !app.session_ready_);
    for(int i=0;i<200 && app.player_->Generation()==generation;++i) usleep(1000);
    assert(app.player_->Generation()>generation && app.close_requested_);
    { std::lock_guard<std::mutex> l(app.control_mu_); app.close_requested_=false; }
    assert(app.gate_->CommitWakeup("fixture")); app.session_ready_=true;
    event(164,1); event(164,2); event(164,0); event(164,1); // Held and bouncing press.
    usleep(30000); assert(callbacks==2 && app.gate_->step()==wakeup::Step::kActive);
    event(164,0); std::this_thread::sleep_for(320ms);
    const auto listening_generation=app.player_->Generation();
    event(164,1); wait(3); // Listening (not speaking) also exits.
    for(int i=0;i<200 && app.player_->Generation()==listening_generation;++i) usleep(1000);
    assert(app.player_->Generation()>listening_generation);
    assert(app.gate_->step()==wakeup::Step::kIdle && app.close_requested_);
    listener.Stop(); close(writer);
  }
};
}
int main() {
  char dir[]="/tmp/xiaoai-stop-key-XXXXXX"; assert(mkdtemp(dir));
  const std::string pcm=std::string(dir)+"/cue.pcm";
  const std::string fake=std::string(dir)+"/aplay";
  {std::ofstream out(fake);out<<"#!/bin/sh\ncat >> \"$XIAOAI_CUE_PCM\"\n";}
  assert(chmod(fake.c_str(),0755)==0);assert(setenv("XIAOAI_CUE_PCM",pcm.c_str(),1)==0);
  const std::string path=std::string(dir)+":"+(getenv("PATH")?getenv("PATH"):"/usr/bin:/bin");
  assert(setenv("PATH",path.c_str(),1)==0);
  app::AppOfflineProbe::Cues(pcm);
  app::AppOfflineProbe::Run(std::string(dir)+"/events");
  std::filesystem::remove_all(dir);
}
