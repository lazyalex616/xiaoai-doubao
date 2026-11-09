#include "audio/recorder.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <poll.h>
#include <thread>
#include <exception>
#include <system_error>

#include <sys/wait.h>
#include <unistd.h>

#include "common/log.hpp"

namespace xiaoai_plus::audio {

namespace {
const auto kLog = xiaoai_plus::GetLogger("recorder");
bool WaitChild(pid_t pid, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (true) {
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) return true;
    if (result < 0 && errno != EINTR) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}
void TerminateChild(pid_t pid) {
  if (pid <= 0) return;
  // waitpid only observes our own child. Never signal after it has been reaped.
  int status = 0;
  const pid_t observed = waitpid(pid, &status, WNOHANG);
  if (observed == pid || (observed < 0 && errno == ECHILD)) return;
  if (observed < 0) { kLog->error("arecord identity check failed"); return; }
  (void)kill(pid, SIGTERM);
  if (WaitChild(pid, std::chrono::seconds(2))) return;
  // A second identity check prevents signalling a reused PID.
  const pid_t still_child = waitpid(pid, &status, WNOHANG);
  if (still_child == 0) {
    (void)kill(pid, SIGKILL);
    if (!WaitChild(pid, std::chrono::seconds(2)))
      kLog->error("arecord could not be reaped after SIGKILL");
  }
}
void NotifyFailure(const std::function<void()>& callback) noexcept {
  try { if (callback) callback(); }
  catch (...) { kLog->error("recorder failure callback threw"); }
}
}  // namespace

ArecordRecorder::ArecordRecorder(config::Audio cfg) : cfg_(std::move(cfg)) {}

ArecordRecorder::~ArecordRecorder() { Stop(); }

bool ArecordRecorder::Start(std::function<void(const std::vector<uint8_t>&)> on_chunk,
                           std::function<void()> on_failure,
                           std::function<void(const char*, size_t)> on_stderr) {
  return StartStamped([on_chunk=std::move(on_chunk)](const auto& chunk,const auto&) {
    if(on_chunk)on_chunk(chunk);
  },std::move(on_failure),std::move(on_stderr));
}
bool ArecordRecorder::StartStamped(
    std::function<void(const std::vector<uint8_t>&,const CaptureStamp&)> on_chunk,
    std::function<void()> on_failure,std::function<void(const char*,size_t)> on_stderr) {
  if (running_.exchange(true)) {
    return true;
  }

  const std::string format = "S" + std::to_string(cfg_.bits_per_sample) + "_LE";
  const std::string sample_rate = std::to_string(cfg_.sample_rate);
  const std::string channels = std::to_string(cfg_.channels);
  const std::string buffer_size = std::to_string(cfg_.buffer_size);
  const std::string period_size = std::to_string(cfg_.period_size);
  const int bytes_per_sample = std::max(1, cfg_.bits_per_sample / 8);
  const int bytes_per_frame = bytes_per_sample * std::max(1, cfg_.channels);
  const int target_chunk = std::max(1, cfg_.buffer_size) * bytes_per_frame;

  kLog->info("starting recorder device={} rate={} channels={} bits={} buffer={} period={} chunk={}",
      cfg_.input_device, cfg_.sample_rate, cfg_.channels, cfg_.bits_per_sample, cfg_.buffer_size,
      cfg_.period_size, target_chunk);

  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) != 0) {
    kLog->error("recorder pipe create failed: {}", std::strerror(errno));
    running_.store(false);
    return false;
  }
  int stderr_pipe[2] = {-1, -1};
  if (on_stderr && pipe(stderr_pipe) != 0) {
    kLog->error("recorder stderr pipe create failed: {}", std::strerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    running_.store(false);
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    kLog->error("recorder fork failed: {}", std::strerror(errno));
    close(pipefd[0]);
    close(pipefd[1]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    running_.store(false);
    return false;
  }

  if (pid == 0) {
    dup2(pipefd[1], STDOUT_FILENO);
    if (stderr_pipe[1] >= 0) dup2(stderr_pipe[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);

    execlp("arecord", "arecord", "--quiet", "-t", "raw", "-D", cfg_.input_device.c_str(),
           "-f", format.c_str(), "-r", sample_rate.c_str(), "-c", channels.c_str(),
           "--buffer-size", buffer_size.c_str(), "--period-size", period_size.c_str(),
           static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipefd[1]);
  pipe_fd_ = pipefd[0];
  if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
  stderr_fd_ = stderr_pipe[0];
  child_pid_ = static_cast<int>(pid);
  ++generation_; // Only after a new physical producer has actually been forked.
  capture_fault_.store(false);

  try {
    if (stderr_fd_ >= 0) {
      stderr_thread_ = std::thread([this, on_stderr = std::move(on_stderr), on_failure]() mutable {
        try { ReadStderrLoop(on_stderr); }
        catch (const std::exception& e) { kLog->error("recorder stderr thread: {}", e.what()); NotifyFailure(on_failure); }
        catch (...) { kLog->error("recorder stderr thread unknown exception"); NotifyFailure(on_failure); }
      });
    }
    thread_ = std::thread([this, on_chunk = std::move(on_chunk),
                           on_failure = std::move(on_failure)]() mutable {
      try { ReadLoop(on_chunk, on_failure); }
      catch (const std::exception& e) { kLog->error("recorder read thread: {}", e.what()); NotifyFailure(on_failure); }
      catch (...) { kLog->error("recorder read thread unknown exception"); NotifyFailure(on_failure); }
    });
  } catch (const std::system_error& e) {
    kLog->error("recorder thread creation failed: {}", e.what());
    running_.store(false);
    TerminateChild(static_cast<pid_t>(child_pid_));
    if (thread_.joinable()) thread_.join();
    if (stderr_thread_.joinable()) stderr_thread_.join();
    close(pipe_fd_);
    if (stderr_fd_ >= 0) close(stderr_fd_);
    pipe_fd_ = -1;
    stderr_fd_ = -1;
    child_pid_ = -1;
    running_.store(false);
    return false;
  }
  return true;
}

void ArecordRecorder::ReadStderrLoop(std::function<void(const char*, size_t)> on_stderr) {
  char buf[256];
  std::string tail;
  const int fd = stderr_fd_;
  while (true) {
    if (!running_.load()) break;
    pollfd ready{fd, POLLIN, 0};
    const int polled = poll(&ready, 1, 100);
    if (polled == 0 || (polled < 0 && errno == EINTR)) continue;
    if (polled < 0) break;
    const ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      tail.append(buf,static_cast<size_t>(n));
      if(tail.find("overrun")!=std::string::npos)capture_fault_.store(true);
      if(tail.size()>1024)tail.erase(0,tail.size()-128);
      on_stderr(buf, static_cast<size_t>(n));
    } else if (n == 0 || errno != EINTR) {
      break;
    }
  }
  on_stderr("", 0);  // Flush a final partial diagnostic line on EOF.
}

void ArecordRecorder::ReadLoop(std::function<void(const std::vector<uint8_t>&,const CaptureStamp&)> on_chunk,
                              std::function<void()> on_failure) {
  const int bytes_per_sample = std::max(1, cfg_.bits_per_sample / 8);
  const int bytes_per_frame = bytes_per_sample * std::max(1, cfg_.channels);
  const size_t target_size =
      static_cast<size_t>(std::max(1, cfg_.buffer_size)) * static_cast<size_t>(bytes_per_frame);
  const size_t read_size =
      static_cast<size_t>(std::max(1, cfg_.period_size)) * static_cast<size_t>(bytes_per_frame);

  std::vector<uint8_t> buf(read_size);
  std::vector<uint8_t> acc;
  acc.reserve(target_size * 2);
  std::vector<uint8_t> chunk(target_size);
  size_t acc_read_pos = 0;
  const uint64_t generation=generation_.load();
  uint64_t sample=0,first_read_ns=0,last_delivery_ns=0;
  auto now_ns=[] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()); };
  while (running_.load()) {
    const int fd = pipe_fd_;
    if (fd < 0) {
      break;
    }

    pollfd ready{fd, POLLIN, 0};
    const int polled = poll(&ready, 1, 100);
    if (polled == 0 || (polled < 0 && errno == EINTR)) continue;
    if (polled < 0) break;
    ssize_t n = read(fd, buf.data(), read_size);
    if (n > 0) {
      const uint64_t acquired_ns=now_ns();
      if(acc.size()==acc_read_pos)first_read_ns=acquired_ns;
      acc.insert(acc.end(), buf.begin(), buf.begin() + static_cast<long>(n));
      while (acc.size() - acc_read_pos >= target_size) {
        std::memcpy(chunk.data(), acc.data() + acc_read_pos, target_size);
        const uint64_t delivered_ns=now_ns();
        // Delivery includes synchronous processing/scheduling time. Late bytes
        // alone do not prove a hardware overrun or broken sample sequence.
        if(last_delivery_ns && delivered_ns-last_delivery_ns>200000000)
          kLog->warn("capture delivery recovered gap_ms={} begin_ns={} recovered_ns={} reason=late_delivery",(delivered_ns-last_delivery_ns)/1000000,last_delivery_ns,delivered_ns);
        const bool continuous=!capture_fault_.load();
        const CaptureStamp stamp{generation,sample,first_read_ns,delivered_ns,continuous};
        on_chunk(chunk,stamp);
        sample+=target_size/static_cast<size_t>(bytes_per_frame);
        last_delivery_ns=delivered_ns;
        acc_read_pos += target_size;
        first_read_ns=acquired_ns; // Any remaining bytes came from this same read.
      }
      if (acc_read_pos == acc.size()) {
        acc.clear();
        acc_read_pos = 0;
      } else if (acc_read_pos >= target_size * 4) {
        acc.erase(acc.begin(), acc.begin() + static_cast<long>(acc_read_pos));
        acc_read_pos = 0;
      }
      continue;
    }

    if (n == 0) {
      if (running_.load()) kLog->error("recorder exited unexpectedly (EOF)");
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (!running_.load()) {
      break;
    }
    if (errno != EBADF) {
      kLog->error("recorder read error: {}", std::strerror(errno));
    }
    break;
  }
  if (running_.load()) NotifyFailure(on_failure);
}

void ArecordRecorder::Stop() {
  if (!running_.exchange(false) && child_pid_ <= 0 && !thread_.joinable() &&
      !stderr_thread_.joinable()) {
    return;
  }

  TerminateChild(static_cast<pid_t>(child_pid_));

  if (thread_.joinable()) {
    thread_.join();
  }
  if (stderr_thread_.joinable()) stderr_thread_.join();

  if (pipe_fd_ >= 0) close(pipe_fd_);
  if (stderr_fd_ >= 0) close(stderr_fd_);
  pipe_fd_ = stderr_fd_ = child_pid_ = -1;
  kLog->info("recorder stopped");
}

}  // namespace xiaoai_plus::audio
