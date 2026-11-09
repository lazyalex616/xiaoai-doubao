#include <cassert>

#include "app/conversation_policy.hpp"

int main() {
  using xiaoai_plus::app::IsExitCommand;
  using xiaoai_plus::app::IsFarewellCommand;
  assert(IsExitCommand("退出"));
  assert(IsExitCommand(" 退出。 "));
  assert(IsExitCommand("退出对话！"));
  assert(IsExitCommand("结束对话?"));
  for (const char* word : {"再见", "拜拜", "豆包再见", "关闭", "关掉",
                           "停止", "停掉", "停下", "退下", "退出", "结束",
                           "结束对话", "不聊了", "别说了", "闭嘴"}) {
    assert(IsExitCommand(word));
    assert(IsExitCommand(std::string("豆包，") + word + "吧！"));
    assert(IsExitCommand(std::string(word) + " 啊。"));
  }
  assert(IsExitCommand("请你先停掉了呀"));
  for(const char* text:{"好了","我猜好了","好 了","停"})assert(!IsExitCommand(text));
  assert(std::string(xiaoai_plus::app::MatchedExitCommand("停下"))=="停下");
  assert(std::string(xiaoai_plus::app::MatchedExitCommand("再见"))=="再见");
  assert(!IsExitCommand("退出按钮在哪里"));
  assert(!IsExitCommand("我不想退出"));
  for(const char* t:{"我们明天再见到恐龙","不要关闭","恐龙说再见","请翻译再见","bye","goodbye","stop","我说的是“再见”"})assert(!IsExitCommand(t));
  assert(IsExitCommand("今天先到这里，再见"));
  assert(!IsExitCommand("再见之前问个问题"));
  assert(!IsExitCommand("这首歌唱了再见之后还有什么"));
  assert(!IsExitCommand("今天我们讲了很多很久很久再见"));
  assert(IsFarewellCommand("再见。"));
  assert(IsFarewellCommand("豆包再见"));
  assert(IsFarewellCommand("拜拜！"));
  assert(!IsFarewellCommand("再见之前问个问题"));
}
