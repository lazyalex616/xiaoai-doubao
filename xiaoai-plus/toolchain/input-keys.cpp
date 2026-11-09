// Standalone read-only key mapping probe, built for the same Linux ABI as evdev.
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <glob.h>
#include <iostream>
#include <linux/input.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <vector>

int main(int argc, char** argv) {
  if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--help")) {
    std::cout << "Usage: xiaoai_input_keys [seconds=20] (1..120)\n"
                 "Lists input nodes; prints EV_KEY down/up/repeat. Read-only; no grab.\n";
    return argc > 2 ? 2 : 0;
  }
  char* end = nullptr;
  const long seconds = argc == 2 ? std::strtol(argv[1], &end, 10) : 20;
  if (seconds < 1 || seconds > 120 || (end && (*end || end == argv[1]))) return 2;
  glob_t nodes{};
  glob("/dev/input/event*", 0, nullptr, &nodes);
  std::vector<pollfd> watched;
  std::vector<std::string> paths;
  for (size_t i = 0; i < nodes.gl_pathc; ++i) {
    const int fd = open(nodes.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    char name[128]{};
    if (fd >= 0) (void)ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    std::cout << nodes.gl_pathv[i] << " name=" << name << (fd < 0 ? " UNREADABLE" : "") << std::endl;
    if (fd >= 0) { watched.push_back({fd, POLLIN, 0}); paths.emplace_back(nodes.gl_pathv[i]); }
  }
  globfree(&nodes);
  if (watched.empty()) { std::cerr << "No readable input nodes\n"; return 1; }
  std::cout << "Press physical play/pause twice, about 2 seconds apart; observe native media too." << std::endl;
  // A monotonic deadline prevents repeated events extending the observation window.
  const auto now_ms = [] { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; };
  const auto deadline = now_ms() + seconds * 1000;
  while (now_ms() < deadline) {
    const int ready = poll(watched.data(), watched.size(), 50);
    if (ready < 0 && errno != EINTR) break;
    for (size_t i = 0; i < watched.size(); ++i) {
      if (watched[i].revents & (POLLHUP | POLLERR | POLLNVAL)) { close(watched[i].fd); watched[i].fd = -1; }
      if (!(watched[i].revents & POLLIN)) continue;
      input_event event{};
      if (read(watched[i].fd, &event, sizeof(event)) == sizeof(event) && event.type == EV_KEY)
        std::cout << paths[i] << " code=" << event.code << " value=" << event.value << std::endl;
    }
  }
  for (const auto& fd : watched) if (fd.fd >= 0) close(fd.fd);
}
