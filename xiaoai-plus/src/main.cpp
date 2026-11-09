#include <csignal>
#include <atomic>
#include <chrono>
#include <exception>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>
#include <thread>

#include "common/log.hpp"

#include "app/app.hpp"
#include "config/config.hpp"
#include "memory/selftest.hpp"

namespace {
volatile std::sig_atomic_t g_stop_requested = 0;

void SignalHandler(int) {
  g_stop_requested = 1;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::string config_path;
  std::string keywords_file;
  bool kws_only = false;
  bool check_config = false;
  bool trial_once = false;
  bool memory_selftest = false;
  bool kws_diagnostics = false;
  int kws_s32_shift = 16;
  int kws_input_channel = -1;
  float kws_threshold = 0.10f;
  bool experimental_pcm_option = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-c" && i + 1 < argc) {
      config_path = argv[++i];
    } else if (arg == "--kws-only") {
      kws_only = true;
    } else if (arg == "--kws-diagnostics") {
      kws_diagnostics = true;
    } else if (arg == "--kws-s32-shift" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "8") kws_s32_shift = 8;
      else if (value == "16") kws_s32_shift = 16;
      else { std::cerr << "--kws-s32-shift must be 8 or 16" << std::endl; return 2; }
      experimental_pcm_option = true;
    } else if (arg == "--kws-input-channel" && i + 1 < argc) {
      const std::string value = argv[++i];
      if (value == "beamformed") kws_input_channel = -1;
      else if (value == "0" || value == "1" || value == "2")
        kws_input_channel = value[0] - '0';
      else { std::cerr << "--kws-input-channel must be beamformed or 0..2" << std::endl; return 2; }
      experimental_pcm_option = true;
    } else if (arg == "--kws-threshold" && i + 1 < argc) {
      const char* value = argv[++i];
      char* end = nullptr;
      errno = 0;
      kws_threshold = std::strtof(value, &end);
      if (errno || end == value || *end || !(kws_threshold > 0.0f && kws_threshold <= 1.0f)) {
        std::cerr << "--kws-threshold must be in (0,1]" << std::endl; return 2;
      }
      experimental_pcm_option = true;
    } else if (arg == "--memory-selftest") {
      memory_selftest = true;
    } else if (arg == "--check-config") {
      check_config = true;
    } else if (arg == "--trial-once") {
      trial_once = true;
    } else if (arg == "--keywords-file" && i + 1 < argc) {
      keywords_file = argv[++i];
    } else {
      std::cerr << "invalid command line" << std::endl;
      return 2;
    }
  }
  if ((kws_only && !config_path.empty()) || (!kws_only && !keywords_file.empty())) {
    std::cerr << "--kws-only cannot read -c; --keywords-file requires --kws-only" << std::endl;
    return 2;
  }
  if (kws_diagnostics && !kws_only) {
    std::cerr << "--kws-diagnostics requires --kws-only" << std::endl;
    return 2;
  }
  if (experimental_pcm_option && (!kws_only || !kws_diagnostics)) {
    std::cerr << "experimental PCM options require --kws-only --kws-diagnostics" << std::endl;
    return 2;
  }
  if (check_config && (kws_only || config_path.empty())) {
    std::cerr << "--check-config requires -c and cannot be combined with --kws-only" << std::endl;
    return 2;
  }
  if (trial_once && (kws_only || check_config || config_path.empty())) {
    std::cerr << "--trial-once requires -c and a normal cloud session" << std::endl;
    return 2;
  }

  if (memory_selftest && (config_path.empty() || kws_only || check_config || trial_once)) {
    std::cerr << "--memory-selftest requires only -c" << std::endl; return 2;
  }
  try {
    auto cfg = kws_only ? xiaoai_plus::config::Config{}
                        : xiaoai_plus::config::load(config_path, memory_selftest);
    if (!keywords_file.empty()) cfg.wakeup.keywords_file = keywords_file;
    if (experimental_pcm_option) {
      cfg.wakeup.kws_threshold = kws_threshold;
      cfg.wakeup.s32_shift = kws_s32_shift;
      cfg.wakeup.input_channel = kws_input_channel;
    }
    if (cfg.memory.secret_file_failed) std::cerr << "memory disabled: credential file invalid" << std::endl;
    if (memory_selftest) {
      return xiaoai_plus::memory::Selftest(cfg.memory,
        xiaoai_plus::memory::HttpsTransport(cfg.memory, cfg.realtime.preset.ca_file), std::cout);
    }
    if (check_config) {
      std::cout << "config valid" << std::endl;
      return 0;
    }
    xiaoai_plus::app::App app(cfg, kws_only, trial_once, kws_diagnostics,
                             cfg.wakeup.s32_shift, cfg.wakeup.input_channel);
    g_stop_requested = 0;
    std::atomic<bool> watcher_done{false};
    std::atomic<bool> watcher_failed{false};

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    std::thread signal_watcher([&app, &watcher_done, &watcher_failed]() {
      try {
        while (!watcher_done.load()) {
          if (g_stop_requested) {
            app.Stop();
            return;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
      } catch (const std::exception& e) {
        std::cerr << "signal watcher failed: " << e.what() << std::endl;
        watcher_failed.store(true);
        app.Stop();
      } catch (...) {
        std::cerr << "signal watcher failed" << std::endl;
        watcher_failed.store(true);
        app.Stop();
      }
    });

    bool run_ok = false;
    try {
      run_ok = app.Run();
    } catch (...) {
      app.Stop();
      watcher_done.store(true);
      if (signal_watcher.joinable()) {
        signal_watcher.join();
      }
      throw;
    }

    watcher_done.store(true);
    if (signal_watcher.joinable()) {
      signal_watcher.join();
    }
    if (!run_ok || watcher_failed.load()) {
      std::cerr << "app stopped with error" << std::endl;
      return 1;
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "startup failed: " << e.what() << std::endl;
    return 1;
  }
}
