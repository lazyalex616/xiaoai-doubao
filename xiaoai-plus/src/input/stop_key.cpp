#include "input/stop_key.hpp"

#include <cerrno>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>
#include "common/log.hpp"

namespace xiaoai_plus::input {
StopKey::StopKey(std::string device, int key_code, std::function<void()> on_press)
    : device_(std::move(device)), key_code_(key_code), on_press_(std::move(on_press)) {}
StopKey::~StopKey() { Stop(); }

bool StopKey::Start() {
  if (device_.empty() || key_code_ <= 0) return true; // Unconfirmed mapping stays disabled.
  if (thread_.joinable()) return false;
  fd_ = open(device_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) return false;
  running_ = true;
  thread_ = std::thread([this] { ReadLoop(); });
  return true;
}
void StopKey::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) { close(fd_); fd_ = -1; }
}
void StopKey::ReadLoop() {
  bool held = false;
  auto last = std::chrono::steady_clock::time_point::min();
  while (running_) {
    pollfd watched{fd_, POLLIN, 0};
    const int ready = poll(&watched, 1, 50);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0 || (watched.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
    if (ready == 0) continue;
    input_event event{};
    const auto count = read(fd_, &event, sizeof(event));
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
    if (count != sizeof(event)) break;
    if (event.type != EV_KEY || event.code != key_code_) continue;
    if (event.value == 0) { held = false; continue; }
    if (event.value != 1 || held) continue; // Ignore autorepeat and duplicate downs.
    held = true;
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point::min() &&
        now - last < std::chrono::milliseconds(300)) continue;
    last = now;
    if (running_ && on_press_) on_press_();
  }
  if (running_.exchange(false))
    GetLogger("stop_key")->warn("input listener stopped; key exit unavailable");
}
}  // namespace xiaoai_plus::input
