#ifdef NDEBUG
#error requires active assertions
#endif
#include "audio/volume.hpp"
#include <cassert>
#include <cstdlib>
#include <unistd.h>

using xiaoai_plus::audio::VolumeControl;
int main() {
  for (const char* text : {"大声点", "声音大一点。", "请大声一点", "请你大声点", "再大声点", "声音调大一点", "加大音量", "调大音量，继续吧", "Louder please", "please louder", "speak louder"})
    assert(VolumeControl::Command(text) == 1);
  for (const char* text : {"小声点", "声音小一点", "麻烦声音小点", "音量调小", "减小音量", "quieter", "Please quieter."})
    assert(VolumeControl::Command(text) == -1);
  for (const char* text : {"我想大声唱歌", "我们小声点说话吧", "I want louder music", "loudermouth", "", "大声唱歌"})
    assert(VolumeControl::Command(text) == 0);
  assert(VolumeControl::Next(255, 1) == 255);
  assert(VolumeControl::Next(144, -1) == 144);
  assert(VolumeControl::Next(40, -1) == 40);
  assert(VolumeControl::Next(160, 1) == 172);
  assert(VolumeControl::Next(200, -1) == 172);
  char dir[] = "/tmp/volume-test-XXXXXX"; assert(mkdtemp(dir));
  const std::string base(dir), mixer = base + "/mixer", state = base + "/volume";
  { std::ofstream out(mixer); out << 160; }
  { std::ofstream out(base + "/amixer"); out << "#!/bin/sh\n"
      "case \"$*\" in\n*cget*) v=$(cat '" << mixer << "'); echo \" : values=$v,$v\";;\n"
      "*cset*) for v; do :; done; echo \"${v%%,*}\" >'" << mixer << "';;\nesac\n"; }
  assert(chmod((base + "/amixer").c_str(), 0700) == 0);
  const std::string old = getenv("PATH"), path = base + ":" + old;
  assert(setenv("PATH", path.c_str(), 1) == 0);
  VolumeControl volume(state); assert(volume.Initialize());
  assert(volume.Adjust(1) && VolumeControl::Read() == 172);
  { std::ifstream in(state); int saved = 0; in >> saved; assert(saved == 172); }
  struct stat st{}; assert(stat(state.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
  { std::ofstream out(mixer); out << 160; } // A reset mixer is restored on restart.
  VolumeControl restarted(state); assert(restarted.Initialize() && VolumeControl::Read() == 172);
  { std::ofstream out(mixer); out << 228; } // Native changes win at the next command.
  assert(restarted.Adjust(-1) && VolumeControl::Read() == 200);
  for (int i = 0; i < 8; ++i) assert(restarted.Adjust(1));
  assert(VolumeControl::Read() == 255);
  for (int i = 0; i < 8; ++i) assert(restarted.Adjust(-1));
  assert(VolumeControl::Read() == 144);
  assert(restarted.Snapshot());
  assert(setenv("PATH", old.c_str(), 1) == 0);
  for (const auto& file : {mixer, state, base + "/amixer"}) assert(unlink(file.c_str()) == 0);
  assert(rmdir(dir) == 0);
}
