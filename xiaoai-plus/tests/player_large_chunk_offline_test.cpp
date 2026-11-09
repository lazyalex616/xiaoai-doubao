#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "audio/player.hpp"
#include "audio/output_format.hpp"

using namespace std::chrono_literals;

int main() {
  char dirname[] = "/tmp/oh2p-slow-aplay-XXXXXX";
  assert(mkdtemp(dirname));
  const std::string fake = std::string(dirname) + "/aplay";
  const std::string output = std::string(dirname) + "/captured.raw";
  {
    std::ofstream out(fake);
    out << "#!/usr/bin/env python3\n"
           "import os,time\n"
           "time.sleep(1.2)\n"
           "with open(os.environ['OH2P_CAPTURE'], 'wb') as dst:\n"
           " while True:\n"
           "  data=os.read(0,4800)\n"
           "  if not data: break\n"
           "  dst.write(data); dst.flush(); time.sleep(0.1)\n";
  }
  assert(chmod(fake.c_str(), 0755) == 0);
  const char* old_path = getenv("PATH");
  const std::string path = std::string(dirname) + ":" + (old_path ? old_path : "/usr/bin:/bin");
  assert(setenv("PATH", path.c_str(), 1) == 0);
  assert(setenv("OH2P_CAPTURE", output.c_str(), 1) == 0);
  xiaoai_plus::config::Audio audio;
  audio.output_device = "ignored";
  audio.sample_rate = 24000;
  audio.channels = 1;
  audio.bits_per_sample = 16;
  xiaoai_plus::audio::AplayPlayer player(audio, 2);
  std::mutex mu;
  std::condition_variable cv;
  int callbacks = 0;
  bool success = false;
  player.SetOnChunkProcessed([&](bool ok) {
    std::lock_guard<std::mutex> lock(mu);
    ++callbacks;
    success = ok;
    cv.notify_one();
  });
  assert(player.Start());
  std::vector<uint8_t> chunk(70 * 1024);
  for (size_t i = 0; i < chunk.size() / 2; ++i) {
    const auto sample = static_cast<int16_t>(std::lround(0.5 * 32767 *
        std::sin(2.0 * 3.141592653589793 * 440.0 * i / 24000)));
    chunk[i * 2] = uint8_t(sample);
    chunk[i * 2 + 1] = uint8_t(uint16_t(sample) >> 8);
  }
  xiaoai_plus::audio::OutputFormat s16_decoder;
  assert(s16_decoder.Inspect(chunk).selected == xiaoai_plus::audio::OutputFormat::Kind::kS16);
  const auto start = std::chrono::steady_clock::now();
  assert(player.Play(s16_decoder.ToS16(chunk)));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 8s, [&] { return callbacks == 1; }));
    assert(success);
  }
  assert(std::chrono::steady_clock::now() - start > 1s);
  std::vector<uint8_t> float_chunk;
  for (size_t i = 0; i < 2400; ++i) {
    const float sample = 0.5f * std::sin(2.0 * 3.141592653589793 * 440.0 * i / 24000);
    uint32_t bits;
    std::memcpy(&bits, &sample, sizeof(bits));
    for (int byte = 0; byte < 4; ++byte) float_chunk.push_back(uint8_t(bits >> (byte * 8)));
  }
  xiaoai_plus::audio::OutputFormat f32_decoder;
  assert(f32_decoder.Inspect(float_chunk).selected == xiaoai_plus::audio::OutputFormat::Kind::kF32);
  const auto converted = f32_decoder.ToS16(float_chunk);
  assert(player.Play(converted));
  {
    std::unique_lock<std::mutex> lock(mu);
    assert(cv.wait_for(lock, 8s, [&] { return callbacks == 2; }));
    assert(success);
  }
  chunk.insert(chunk.end(), converted.begin(), converted.end());
  for (int i = 0; i < 50; ++i) {
    struct stat info {};
    if (stat(output.c_str(), &info) == 0 && static_cast<size_t>(info.st_size) == chunk.size()) break;
    std::this_thread::sleep_for(100ms);
  }
  player.Close();
  std::ifstream in(output, std::ios::binary);
  std::vector<uint8_t> captured((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  assert(captured == chunk);
  assert(unlink(fake.c_str()) == 0);
  assert(unlink(output.c_str()) == 0);
  assert(rmdir(dirname) == 0);
}
