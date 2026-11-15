#include "audio/player.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <vector>

#include "common/log.hpp"

extern char** environ;

namespace xiaoai_plus::audio {

namespace {
const auto kLog = xiaoai_plus::GetLogger("player");
}  // namespace

AplayPlayer::AplayPlayer(config::Audio cfg, int queue_frames)
    : cfg_(std::move(cfg)), queue_frames_(queue_frames > 0 ? queue_frames : 128) {}

AplayPlayer::~AplayPlayer() { Close(); }

bool AplayPlayer::Start() {
  if (running_.exchange(true)) {
    return true;
  }

  try {
    thread_ = std::thread([this]() {
      try { WriteLoop(); }
      catch (const std::exception& e) {
        kLog->error("player thread: {}", e.what());
        std::function<void()> fatal;
        { std::lock_guard<std::mutex> lock(cb_mu_); fatal = on_fatal_; }
        if (fatal) fatal();
      } catch (...) {
        kLog->error("player thread unknown exception");
        std::function<void()> fatal;
        { std::lock_guard<std::mutex> lock(cb_mu_); fatal = on_fatal_; }
        if (fatal) fatal();
      }
    });
  } catch (const std::system_error& e) {
    kLog->error("player thread creation failed: {}", e.what());
    running_.store(false);
    return false;
  }
  kLog->info("player started");
  return true;
}

bool AplayPlayer::Play(const std::vector<uint8_t>& chunk) {
  return Play(chunk, Generation());
}

bool AplayPlayer::Play(const std::vector<uint8_t>& chunk, uint64_t generation, bool notify) {
  if (chunk.empty()) {
    return true;
  }

  std::lock_guard<std::mutex> lock(mu_);
  if (!running_.load() || generation != Generation()) return false;
  if (static_cast<int>(queue_.size()) >= queue_frames_) {
    return false;
  }
  queue_.push_back(Chunk{chunk, generation, notify});
  cv_.notify_one();
  return true;
}

void AplayPlayer::Interrupt() { (void)Interrupt(Generation()); }

bool AplayPlayer::Interrupt(uint64_t generation) {
  {
    // Publish cancellation before waiting for pipe_mu. A large/slow chunk can
    // now stop at its next partial write or bounded poll return.
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    std::lock_guard<std::mutex> lock(mu_);
    if (generation != Generation()) return false;
    ++generation_;
    queue_.clear();
  }
  cv_.notify_all();
  std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
  if (pipe_generation_ == generation) ClosePipeLocked(true);
  return true;
}

bool AplayPlayer::Drain(uint64_t generation, std::chrono::milliseconds timeout) {
  const auto deadline=std::chrono::steady_clock::now()+timeout;
  {std::unique_lock<std::mutex> lock(mu_);
    // A completion callback on the writer must never wait for its own queue.
    if(std::this_thread::get_id()==thread_.get_id() && (!queue_.empty() || writing_))return false;
    if(!cv_.wait_until(lock,deadline,[&] {
      return generation!=Generation() || (queue_.empty() && !writing_);
    }) || generation!=Generation())return false;}
  std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
  if(aplay_pid_<=0)return true;
  const auto pid=aplay_pid_;
  close(aplay_fd_);aplay_fd_=-1;aplay_pid_=-1;pipe_generation_=0;
  while(std::chrono::steady_clock::now()<deadline && generation==Generation()) {
    const auto result=waitpid(pid,nullptr,WNOHANG);
    if(result==pid)return true;
    if(result<0 && errno!=EINTR)return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  kill(pid,SIGKILL);
  while(waitpid(pid,nullptr,0)<0 && errno==EINTR){}
  return false;
}

void AplayPlayer::SetOnScopedChunkProcessed(std::function<void(uint64_t, ChunkResult)> cb) {
  std::lock_guard<std::mutex> lock(cb_mu_);
  on_scoped_chunk_processed_ = std::move(cb);
}

void AplayPlayer::SetOnChunkProcessed(std::function<void(bool)> cb) {
  std::lock_guard<std::mutex> lock(cb_mu_);
  on_chunk_processed_ = std::move(cb);
}

void AplayPlayer::SetOnWriteProgress(std::function<void()> cb) {
  std::lock_guard<std::mutex> lock(cb_mu_);
  on_write_progress_ = std::move(cb);
}

void AplayPlayer::SetOnFatal(std::function<void()> cb) {
  std::lock_guard<std::mutex> lock(cb_mu_);
  on_fatal_ = std::move(cb);
}

bool AplayPlayer::OpenPipeLocked() {
  if (aplay_fd_ >= 0) {
    return true;
  }

  std::string fmt = "S" + std::to_string(cfg_.bits_per_sample) + "_LE";
  std::string rate = std::to_string(cfg_.sample_rate);
  std::string channels = std::to_string(cfg_.channels);
  std::string buffer_size = std::to_string(cfg_.buffer_size);
  std::string period_size = std::to_string(cfg_.period_size);

  std::vector<char*> argv = {
      const_cast<char*>("aplay"),
      const_cast<char*>("--quiet"),
      const_cast<char*>("-t"),           const_cast<char*>("raw"),
      const_cast<char*>("-D"),           const_cast<char*>(cfg_.output_device.c_str()),
      const_cast<char*>("-f"),           const_cast<char*>(fmt.c_str()),
      const_cast<char*>("-r"),           const_cast<char*>(rate.c_str()),
      const_cast<char*>("-c"),           const_cast<char*>(channels.c_str()),
      const_cast<char*>("--buffer-size"), const_cast<char*>(buffer_size.c_str()),
      const_cast<char*>("--period-size"), const_cast<char*>(period_size.c_str()),
      const_cast<char*>("-"),
      nullptr,
  };

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    kLog->error("player pipe() failed");
    return false;
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    kLog->error("player spawn actions failed");
    return false;
  }
  posix_spawn_file_actions_adddup2(&actions, pipefd[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&actions, pipefd[0]);
  posix_spawn_file_actions_addclose(&actions, pipefd[1]);
  pid_t pid = -1;
  const int spawn_rc = posix_spawnp(&pid, "aplay", &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawn_rc != 0 || pid <= 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    kLog->error("player spawn failed");
    return false;
  }

  // parent
  close(pipefd[0]);
  const int flags = fcntl(pipefd[1], F_GETFL);
  if (flags < 0 || fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) < 0) {
    kill(pid, SIGKILL);
    close(pipefd[1]);
    (void)waitpid(pid, nullptr, WNOHANG);
    return false;
  }
  aplay_pid_ = pid;
  aplay_fd_ = pipefd[1];
  return true;
}

void AplayPlayer::ClosePipeLocked(bool kill_now) {
  const pid_t pid = aplay_pid_;
  const int fd = aplay_fd_;
  aplay_pid_ = -1;
  aplay_fd_ = -1;
  pipe_generation_ = 0;

  if (pid > 0 && kill_now) {
    kill(pid, SIGKILL);
  }
  if (fd >= 0) {
    close(fd);
  }
  if (pid > 0) {
    bool reaped = false;
    for (int i = 0; i < 20; ++i) {
      const pid_t result = waitpid(pid, nullptr, WNOHANG);
      if (result == pid || result < 0) { reaped = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!reaped) {
      kill(pid, SIGKILL);
      for (int i = 0; i < 20; ++i) {
        const pid_t result = waitpid(pid, nullptr, WNOHANG);
        if (result == pid || result < 0) { reaped = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!reaped) kLog->warn("aplay did not exit within 2s; deferred OS reap");
    }
  }
}

void AplayPlayer::WaitAudible(uint64_t generation) {
  using Clock = std::chrono::steady_clock;
  const auto began = Clock::now();
  const auto deadline = began + std::chrono::seconds(3);
  auto tail_deadline = deadline;
  bool empty = false;
  int first_bytes = -1;
  const auto tail = std::chrono::milliseconds(
      200 + 1000 * cfg_.buffer_size / std::max(1, cfg_.sample_rate));
  while (Clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!queue_.empty() || generation != Generation() || !running_.load()) break;
    }
    {
      std::lock_guard<std::mutex> lock(pipe_mu_);
      if (aplay_fd_ < 0 || pipe_generation_ != generation) break;
      if (!empty) {
        int bytes = 0;
        if (ioctl(aplay_fd_, FIONREAD, &bytes) < 0) {
          kLog->warn("player audible ioctl failed errno={}", errno);
          break;
        }
        if (first_bytes < 0) first_bytes = bytes;
        if (bytes == 0) { empty = true; tail_deadline = Clock::now() + tail; }
      }
    }
    if (empty && Clock::now() >= tail_deadline) break;
    // Play/Interrupt/Close wake the wait without holding any pipe or callback lock.
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
      return !queue_.empty() || generation != Generation() || !running_.load();
    });
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-began).count();
  if (ms >= 200) kLog->info("playback audible wait_ms={} pipe_bytes={}", ms, first_bytes);
}

void AplayPlayer::WriteLoop() {
  // Aplay may exit between spawn and write. Keep EPIPE local to this writer
  // instead of letting SIGPIPE terminate the paid-session process.
  sigset_t pipe_signal;
  sigemptyset(&pipe_signal);
  sigaddset(&pipe_signal, SIGPIPE);
#ifdef XIAOAI_TEST_SIGPIPE_BLOCK_FAILURE
  const int mask_result = EINVAL;
#else
  const int mask_result = pthread_sigmask(SIG_BLOCK, &pipe_signal, nullptr);
#endif
  if (mask_result != 0) {
    kLog->error("player could not block SIGPIPE");
    running_.store(false);
    std::function<void(bool)> cb;
    {
      std::lock_guard<std::mutex> lock(cb_mu_);
      cb = on_chunk_processed_;
    }
    if (cb) cb(false);
    return;
  }
  const auto idle_timeout = std::chrono::milliseconds(1200);
  while (running_.load()) {
    Chunk item;
    std::vector<uint8_t> chunk;
    bool timed_out = false;
    {
      std::unique_lock<std::mutex> lock(mu_);
      if (!cv_.wait_for(lock, idle_timeout, [this]() { return !running_.load() || !queue_.empty(); })) {
        timed_out = true;
      }
      if (!running_.load()) {
        break;
      }
      if (timed_out && queue_.empty()) {
        lock.unlock();
        std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
        ClosePipeLocked(false);
        continue;
      }
      item = std::move(queue_.front());
      chunk = std::move(item.bytes);
      queue_.pop_front();writing_=true;
    }

    if (chunk.empty()) {
      {std::lock_guard<std::mutex> lock(mu_);writing_=false;}cv_.notify_all();
      continue;
    }

    bool write_ok = false;
    bool canceled = item.generation != Generation();
    {
      std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
      if (aplay_fd_ >= 0 && pipe_generation_ != item.generation) ClosePipeLocked(true);
      const bool newly_opened = aplay_fd_ < 0;
      if (!canceled && OpenPipeLocked()) {
        pipe_generation_ = item.generation;
        size_t offset = 0;
        const size_t bytes_per_100ms = static_cast<size_t>(cfg_.sample_rate) *
            static_cast<size_t>(cfg_.channels) * static_cast<size_t>(cfg_.bits_per_sample / 8) / 10;
        const size_t segment_bytes = std::max<size_t>(2, bytes_per_100ms);
        auto last_progress = std::chrono::steady_clock::now();
        bool failed = false;
        while (offset < chunk.size()) {
          if (!running_.load() || item.generation != Generation()) {
            canceled = true;
            break;
          }
          if (std::chrono::steady_clock::now() - last_progress >= std::chrono::seconds(3)) {
            failed = true;
            break;
          }
          const size_t write_size = std::min(segment_bytes, chunk.size() - offset);
          const ssize_t written = write(aplay_fd_, chunk.data() + offset, write_size);
          if (written > 0) {
            offset += static_cast<size_t>(written);
            last_progress = std::chrono::steady_clock::now();
            std::function<void()> progress;
            {
              std::lock_guard<std::mutex> lock(cb_mu_);
              progress = on_write_progress_;
            }
            if (progress && item.notify) {
              std::lock_guard<std::recursive_mutex> fence(callback_fence_);
              if (item.generation == Generation()) progress();
            }
            if (newly_opened && offset == static_cast<size_t>(written))
              kLog->info("first_aplay_write_at_ns={} bytes={}",
                         std::chrono::duration_cast<std::chrono::nanoseconds>(
                             last_progress.time_since_epoch()).count(), written);
            continue;
          }
          if (written < 0 && errno == EINTR) continue;
          if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd pfd{aplay_fd_, POLLOUT, 0};
            const int ready = poll(&pfd, 1, 50);
            if (!running_.load() || item.generation != Generation()) {
              canceled = true;
              break;
            }
            if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) { failed = true; break; }
            if (ready >= 0) continue;
            if (errno == EINTR) continue;
          }
          failed = true;
          break;
        }
        canceled = canceled || item.generation != Generation() || !running_.load();
        write_ok = !canceled && !failed && offset == chunk.size();
        if (canceled) {
          ClosePipeLocked(true);
        } else if (!write_ok) {
          kLog->warn("player write timed out or failed (written={}/{})", offset, chunk.size());
          ClosePipeLocked(true);
        }
      }
    }

    {std::lock_guard<std::mutex> lock(mu_);writing_=false;}cv_.notify_all();
    if(!item.notify)continue;
    if(write_ok && !canceled)WaitAudible(item.generation);
    std::function<void(bool)> cb;
    {
      std::lock_guard<std::mutex> lock(cb_mu_);
      cb = on_chunk_processed_;
    }
    std::function<void(uint64_t, ChunkResult)> scoped;
    { std::lock_guard<std::mutex> lock(cb_mu_); scoped = on_scoped_chunk_processed_; }
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    canceled = canceled || item.generation != Generation();
    if (scoped) scoped(item.generation, canceled ? ChunkResult::kCanceled :
                       write_ok ? ChunkResult::kWritten : ChunkResult::kFailed);
    // The recursive fence deliberately allows a scoped callback to Interrupt
    // synchronously. Its return must not carry a stale completion into legacy.
    canceled = canceled || item.generation != Generation() || !running_.load();
    // Intentional cancellation is not a playback failure, and a stale chunk
    // must not decrement the legacy App's new-turn pending count.
    if (!canceled && cb) cb(write_ok);
  }
}

void AplayPlayer::Close() {
  bool was_running;
  {
    // Publish shutdown through the same completion fence as Interrupt. Release
    // it before joining, so the writer can finish a pending callback normally.
    std::lock_guard<std::recursive_mutex> fence(callback_fence_);
    was_running = running_.exchange(false);
    ++generation_;
  }
  if (!was_running && !thread_.joinable()) return;

  cv_.notify_all();

  if (thread_.joinable()) {
    thread_.join();
  }

  {
    std::lock_guard<std::mutex> pipe_lock(pipe_mu_);
    ClosePipeLocked(true);
  }

  std::lock_guard<std::mutex> lock(mu_);
  queue_.clear();
  kLog->info("player stopped");
}

}  // namespace xiaoai_plus::audio
