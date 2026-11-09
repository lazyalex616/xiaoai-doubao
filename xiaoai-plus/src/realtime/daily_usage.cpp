#include "realtime/daily_usage.hpp"

#include <cerrno>
#include <climits>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xiaoai_plus::realtime {
namespace {
struct Fd {
  int value{-1};
  explicit Fd(int fd) : value(fd) {}
  ~Fd() { if (value >= 0) close(value); }
};

std::string Today() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (now == static_cast<std::time_t>(-1) || !localtime_r(&now, &local))
    throw std::runtime_error("usage clock unavailable");
  char date[16];
  if (std::strftime(date, sizeof(date), "%Y-%m-%d", &local) == 0)
    throw std::runtime_error("usage date unavailable");
  return date;
}
}  // namespace

uint64_t DailyUsage::ReadToday() const { return Access(0, false); }
uint64_t DailyUsage::AddToday(uint64_t tokens) const { return Access(tokens, true); }

uint64_t DailyUsage::Access(uint64_t add, bool update) const {
  if (directory_.empty()) throw std::runtime_error("usage directory missing");
  if (mkdir(directory_.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("usage directory unavailable");
  Fd dir(open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  struct stat st{};
  if (dir.value < 0 || fstat(dir.value, &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0)
    throw std::runtime_error("usage directory is not private");
  const std::string date = Today();
  Fd file(openat(dir.value, date.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600));
  if (file.value < 0 || fstat(file.value, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != geteuid() || (st.st_mode & 0077) != 0 || flock(file.value, LOCK_EX) != 0)
    throw std::runtime_error("usage file is not private");
  char buf[32]{};
  const ssize_t n = read(file.value, buf, sizeof(buf));
  if (n < 0 || n >= static_cast<ssize_t>(sizeof(buf)))
    throw std::runtime_error("usage file invalid");
  uint64_t total = 0;
  if (n > 0) {
    size_t digits = static_cast<size_t>(n);
    if (buf[digits - 1] == '\n') --digits;
    if (digits == 0) throw std::runtime_error("usage file invalid");
    for (size_t i = 0; i < digits; ++i) {
      if (buf[i] < '0' || buf[i] > '9' || total > (UINT64_MAX - (buf[i] - '0')) / 10)
        throw std::runtime_error("usage file invalid");
      total = total * 10 + static_cast<uint64_t>(buf[i] - '0');
    }
    if (digits + (buf[digits] == '\n' ? 1 : 0) != static_cast<size_t>(n))
      throw std::runtime_error("usage file invalid");
  }
  if (update) {
    if (total > UINT64_MAX - add) throw std::runtime_error("usage overflow");
    total += add;
    const std::string value = std::to_string(total) + "\n";
    if (lseek(file.value, 0, SEEK_SET) < 0 ||
        write(file.value, value.data(), value.size()) != static_cast<ssize_t>(value.size()) ||
        ftruncate(file.value, value.size()) != 0 || fsync(file.value) != 0)
      throw std::runtime_error("usage write failed");
  }
  return total;
}

}  // namespace xiaoai_plus::realtime
