#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "realtime/daily_usage.hpp"

int main() {
  char root[] = "/tmp/xiaoai-daily-usage-XXXXXX";
  assert(mkdtemp(root));
  const std::string directory = std::string(root) + "/usage";
  xiaoai_plus::realtime::DailyUsage usage(directory);
  assert(usage.ReadToday() == 0);
  assert(usage.AddToday(7) == 7);
  assert(usage.AddToday(11) == 18);
  assert(usage.ReadToday() == 18);
  struct stat st{};
  assert(stat(directory.c_str(), &st) == 0 && (st.st_mode & 0077) == 0);
  bool rejected = false;
  assert(chmod(directory.c_str(), 0755) == 0);
  try { (void)usage.ReadToday(); } catch (...) { rejected = true; }
  assert(rejected);
  assert(chmod(directory.c_str(), 0700) == 0);
  const std::string link = std::string(root) + "/link";
  assert(symlink(directory.c_str(), link.c_str()) == 0);
  rejected = false;
  try { (void)xiaoai_plus::realtime::DailyUsage(link).ReadToday(); }
  catch (...) { rejected = true; }
  assert(rejected);
  std::filesystem::remove_all(root);
}
