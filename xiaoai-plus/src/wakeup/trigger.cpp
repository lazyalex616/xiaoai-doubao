#include "wakeup/trigger.hpp"

#include <algorithm>
#include <cctype>

namespace xiaoai_plus::wakeup {

namespace {

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

}  // namespace

Trigger::Trigger(std::vector<std::string> keywords,
                 std::function<bool(const std::string&)> on_wake)
    : allowed_(NormalizeKeywords(keywords)), on_wake_(std::move(on_wake)) {}

std::string Trigger::NormalizeKeyword(const std::string& text) {
  std::string s = ToLower(text);
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (!std::isspace(static_cast<unsigned char>(c))) {
      out.push_back(c);
    }
  }
  return out;
}

bool Trigger::Allows(const std::string& text) const {
  return allowed_.find(NormalizeKeyword(text)) != allowed_.end();
}

bool Trigger::FireFromText(const std::string& text) {
  const auto norm = NormalizeKeyword(text);
  if (norm.empty()) {
    return false;
  }

  // The KWS engine returns a configured keyword label, not a transcript.
  // Substring matching makes @豆包豆包 ambiguously match @豆包 and can
  // accidentally accept a different label containing the wake word.
  const auto it = allowed_.find(norm);
  if (!on_wake_ || it == allowed_.end()) {
    return false;
  }
  return on_wake_(it->second);
}

std::unordered_map<std::string, std::string> Trigger::NormalizeKeywords(
    const std::vector<std::string>& keywords) {
  std::unordered_map<std::string, std::string> out;
  for (const auto& keyword : keywords) {
    auto norm = NormalizeKeyword(keyword);
    if (norm.empty()) {
      continue;
    }
    out[norm] = keyword;
  }
  return out;
}

}  // namespace xiaoai_plus::wakeup
