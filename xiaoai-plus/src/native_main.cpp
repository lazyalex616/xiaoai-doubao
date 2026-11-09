#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <iostream>
#include <string>
#include <thread>

#include "app/native_app.hpp"
#include "config/config.hpp"

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void HandleSignal(int) { stop_requested = 1; }
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::string config_path;
  bool check_config = false;
  bool self_test = false;
  bool trial_once = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "-c" && i + 1 < argc) config_path = argv[++i];
    else if (arg == "--check-config") check_config = true;
    else if (arg == "--self-test") self_test = true;
    else if (arg == "--trial-once") trial_once = true;
    else { std::cerr << "invalid command line" << std::endl; return 2; }
  }
  if (self_test) {
    if (!config_path.empty() || check_config || trial_once) return 2;
    std::cout << "native binary self-test ok; no socket or cloud" << std::endl;
    return 0;
  }
  if (config_path.empty() || (check_config && trial_once)) {
    std::cerr << "-c is required; --check-config cannot be combined with --trial-once"
              << std::endl;
    return 2;
  }
  try {
    auto config = xiaoai_plus::config::load(config_path);
    if (check_config) {
      std::cout << "config valid" << std::endl;
      return 0;
    }
    xiaoai_plus::app::NativeApp app(std::move(config), trial_once);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::atomic<bool> finished{false};
    std::thread watcher([&] {
      while (!finished.load()) {
        if (stop_requested) { app.RequestStop(); return; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    const bool okay = app.Run();
    finished.store(true);
    watcher.join();
    return okay ? 0 : 1;
  } catch (const std::exception&) {
    // Avoid printing config values or server diagnostics.
    std::cerr << "native app startup failed" << std::endl;
    return 1;
  }
}
