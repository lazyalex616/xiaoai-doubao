#pragma once
#include <functional>
#include <string>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "realtime/duplex_protocol.hpp"

namespace xiaoai_plus::memory {
using Json = nlohmann::json;
inline constexpr size_t kCacheLimit = 100*1024;
inline constexpr size_t kSessionLimit = 256*1024;
inline constexpr size_t kSpoolLimit = 2*1024*1024;
inline constexpr size_t kQueueLimit = 128;
std::string IdentityLogText(const std::string& raw);
std::string Redact(std::string text);
inline constexpr size_t kInstructionsBudget = 8192;
std::string BuildInstructions(const Json& child = Json::object(), const std::string& identity = "",
                              const std::string& recent = "", bool short_resume = false, const std::vector<std::string>& names = {});
std::string IdentityQuestion(const std::vector<std::string>& names);
Json SeedNames(const std::vector<std::string>& names);
std::string PlaymateInstructions(const std::vector<std::string>& names);
std::string IdentityInstructions(const std::vector<std::string>& names = {});
struct Candidate { std::string child_id; double confidence{0}; };
struct Decision { bool changed{false}; std::string kind; std::string child_id; std::string greeting; };
// Child access requires a registered name/alias after normalization; no model guesses.
class Identity {
 public:
  using Resolver = std::function<std::string(const std::string&, const Json&)>;
  explicit Identity(std::string question = "Hi! What's your name? Or are you a grown-up?", Resolver resolver = {});
  void Reset(Json children);
  // Refresh the registered-name facts without reopening an already identified actor.
  void UpdateChildren(Json children) { children_ = std::move(children); }
  Decision Hear(const std::string& text, Candidate candidate = {});
  std::string Rule(const std::string& text) const;
  bool IsQuestionTail(const std::string& text) const;
  const std::string& kind() const { return kind_; }
  const std::string& child_id() const { return child_id_; }
  const std::string& question() const { return question_; }
 private:
  std::string question_, kind_{"unknown"}, child_id_;
  int attempt_{0};
  Json children_ = Json::array();
  Resolver resolver_;
};
class Transcript {
 public:
  bool Add(const realtime::duplex::Event& event);
  void Interrupted(const std::string& response_id = "");
  const Json& turns() const { return turns_; }
  bool complete() const { return complete_; }
  void Incomplete() { complete_ = false; }
 private:
  Json turns_ = Json::array();
  std::unordered_set<std::string> events_, keys_;
  bool complete_{true};
  bool interrupted_pending_{false};
};
}  // namespace xiaoai_plus::memory
