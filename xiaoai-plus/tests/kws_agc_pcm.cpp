// Host-only S16LE stdin/stdout adapter for reference parity and KWS replay.
#include <iostream>
#include <array>
#include "audio/kws_preprocess.hpp"
int main() {
  xiaoai_plus::audio::KwsAgc agc;
  std::array<int16_t, 1440> frame{};
  while (std::cin) {
    std::cin.read(reinterpret_cast<char*>(frame.data()), sizeof(frame));
    auto n = std::cin.gcount();
    if (n % 2) return 2;
    agc.Process(frame.data(), n / 2);
    std::cout.write(reinterpret_cast<char*>(frame.data()), n);
  }
  return std::cout ? 0 : 1;
}
