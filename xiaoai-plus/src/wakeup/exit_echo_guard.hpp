#pragma once
#include <array>
#include <mutex>
#include <string>
#include "app/conversation_policy.hpp"

namespace xiaoai_plus::wakeup {
// Distinct internal labels preserve the unchanged on-device keyword file.
inline std::string StrongExitWord(const std::string& label) {
  if (label.rfind("强退:", 0) == 0) return label.substr(std::string("强退:").size());
  return {};
}
inline std::string StrongExitLabel(const std::string& line) {
  for (const auto& pair : {std::pair<const char*, const char*>{"b ì z uǐ", "闭嘴"},
       {"b ié sh uō l e", "别说了"}, {"t íng x ià", "停下"},
       {"g uān b ì", "关闭"}, {"t uì x ià", "退下"}})
    if (line.rfind(pair.first, 0) == 0) return std::string("强退:") + pair.second;
  return {};
}
class ExitEchoGuard {
 public:
  void Reset() { std::lock_guard<std::mutex> l(mu_); Clear(); }
  void Update(const std::string& id, const std::string& text, bool done, bool created=false) {
    std::lock_guard<std::mutex> l(mu_);
    if (created || (!id.empty() && id != id_)) Clear();
    if (!id.empty()) id_=id;
    // Keep flags for the complete reply; only a small suffix is needed for
    // words split across deltas. No conversation text is persisted or logged.
    if (done) { seen_.fill(false); tail_.clear(); }
    auto joined=tail_+app::NormalizeConversationCommand(text);
    const std::array<std::string,5> words={"闭嘴","别说了","停下","关闭","退下"};
    for(size_t i=0;i<words.size();++i) if(joined.find(words[i])!=std::string::npos) seen_[i]=true;
    if(joined.find("关掉")!=std::string::npos) seen_[3]=true;
    for(const char* word:{"停止","停掉"}) if(joined.find(word)!=std::string::npos) seen_[2]=true;
    tail_=joined.substr(joined.size()>32?joined.size()-32:0);
  }
  bool Contains(const std::string& label) {
    std::lock_guard<std::mutex> l(mu_);
    const auto word=StrongExitWord(label);
    const std::array<std::string,5> words={"闭嘴","别说了","停下","关闭","退下"};
    for(size_t i=0;i<words.size();++i) if(word==words[i]) return seen_[i];
    return false;
  }
 private:
  void Clear() { id_.clear();tail_.clear();seen_.fill(false); }
  std::mutex mu_;
  std::string id_,tail_;
  std::array<bool,5> seen_{};
};
} // namespace xiaoai_plus::wakeup
