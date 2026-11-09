#pragma once

#include <string>

namespace xiaoai_plus::app {

inline std::string NormalizeConversationCommand(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n' &&
          c != '.' && c != ',' && c != '!' && c != '?' && c != ';' && c != ':')
        out.push_back(text[i]);
      ++i;
      continue;
    }
    const size_t width = c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    const auto ch = text.substr(i, width);
    if (ch != "。" && ch != "，" && ch != "！" && ch != "？" &&
        ch != "、" && ch != "；" && ch != "：" && ch != "～" &&
        ch != "（" && ch != "）") out += ch;
    i += width;
  }
  return out;
}

inline size_t Utf8Characters(const std::string& text) {
  size_t count = 0;
  for (unsigned char c : text) if ((c & 0xc0) != 0x80) ++count;
  return count;
}

inline const char* MatchedExitCommand(const std::string& text) {
  static const char* commands[] = {
      "再见", "拜拜", "豆包再见", "关闭", "关掉", "停止", "停掉",
      "停下", "退下", "退出", "结束", "结束对话", "不聊了", "别说了",
      "闭嘴", "退出对话"};
  auto normalized = NormalizeConversationCommand(text);
  // Final Chinese transcript only: <=8 characters after punctuation removal,
  // whole command or a command suffix; negation/quotation/metalinguistic requests
  // never exit. No English commands. Long narratives cannot match a suffix.
  if(Utf8Characters(normalized)>8)return nullptr;
  for(const char* deny : {"不想", "不要", "不能", "别关闭", "别退出", "别结束", "不会", "不许", "不是", "没有", "怎么", "如何", "什么", "说再见", "说拜拜", "叫再见", "叫拜拜", "“", "”", "\"", "翻译"}) {
    if(normalized.find(deny)!=std::string::npos)return nullptr;
  }
  for(int pass=0;pass<4;++pass) {
    for(const char* command:commands) {
      const std::string word(command);
      if(normalized==word || (normalized.size()>=word.size() &&
          normalized.compare(normalized.size()-word.size(),word.size(),word)==0))return command;
    }
    bool stripped=false;
    for(const char* particle:{"吧","啊","了","呀","啦","哦","喽"}) {
      const std::string tail(particle);
      if(normalized.size()>=tail.size() && normalized.compare(normalized.size()-tail.size(),tail.size(),tail)==0) {
        normalized.resize(normalized.size()-tail.size());stripped=true;break;
      }
    }
    if(!stripped)break;
  }
  return nullptr;
}

inline bool IsExitCommand(const std::string& text) { return MatchedExitCommand(text)!=nullptr; }

inline bool IsFarewellCommand(const std::string& text) { return IsExitCommand(text); }

}  // namespace xiaoai_plus::app
