#include <cassert>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <string>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#include "audio/recorder.hpp"

using namespace std::chrono_literals;

int main() {
  char directory[] = "/tmp/xiaoai-recorder-XXXXXX";
  assert(mkdtemp(directory));
  const std::string script = std::string(directory) + "/arecord";
  const std::string pid_path = std::string(directory) + "/pid";
  {
    std::ofstream out(script);
    out << "#!/bin/sh\ntrap '' TERM\necho $$ > '" << pid_path
        << "'\nwhile :; do sleep 1; done\n";
  }
  assert(chmod(script.c_str(), 0700) == 0);
  const char* old_path = getenv("PATH");
  const std::string path = std::string(directory) + ":" + (old_path ? old_path : "");
  assert(setenv("PATH", path.c_str(), 1) == 0);

  xiaoai_plus::config::Audio cfg;
  cfg.input_device = "fake";
  cfg.bits_per_sample = 16;
  cfg.channels = 1;
  cfg.sample_rate = 16000;
  cfg.buffer_size = 4;
  cfg.period_size = 4;
  xiaoai_plus::audio::ArecordRecorder recorder(cfg);
  assert(recorder.Start([](const std::vector<uint8_t>&) {}, [] {}));
  pid_t pid = -1;
  for (int i = 0; i < 100; ++i) {
    std::ifstream in(pid_path);
    if (in >> pid) break;
    std::this_thread::sleep_for(10ms);
  }
  assert(pid > 0);
  const auto start = std::chrono::steady_clock::now();
  recorder.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  assert(elapsed >= 2s && elapsed < 5s);
  assert(kill(pid, 0) < 0 && errno == ESRCH);
  {
    std::ofstream out(script);
    out << "#!/bin/sh\nprintf '\\001\\002\\003\\004\\005\\006\\007\\010'\n";
  }
  assert(chmod(script.c_str(), 0700) == 0);
  std::atomic<bool> failed{false};
  assert(recorder.Start([](const std::vector<uint8_t>&) {
    throw std::runtime_error("injected read callback failure");
  }, [&] { failed.store(true); }));
  for (int i = 0; i < 100 && !failed.load(); ++i) std::this_thread::sleep_for(10ms);
  assert(failed.load());
  recorder.Stop();
  unlink(script.c_str());
  unlink(pid_path.c_str());
  rmdir(directory);
}
