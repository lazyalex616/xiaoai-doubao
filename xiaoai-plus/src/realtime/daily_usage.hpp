#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace xiaoai_plus::realtime {

// A local accounting guard. A session already in progress may exceed the limit.
class DailyUsage {
 public:
  explicit DailyUsage(std::string directory) : directory_(std::move(directory)) {}
  uint64_t ReadToday() const;
  uint64_t AddToday(uint64_t tokens) const;

 private:
  uint64_t Access(uint64_t add, bool update) const;
  std::string directory_;
};

}  // namespace xiaoai_plus::realtime
