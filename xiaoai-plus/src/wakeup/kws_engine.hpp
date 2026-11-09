#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace xiaoai_plus::wakeup {

struct KwsHit {
  std::string keyword;
  float score{0};
};

class IKwsEngine {
 public:
  virtual ~IKwsEngine() = default;
  virtual std::optional<KwsHit> AcceptPcm16(const uint8_t* pcm, size_t size_bytes, int sample_rate,
                                            int channels, int bits_per_sample) = 0;
  virtual void Reset() = 0;
  // Exit words (@结束) are decoded only while enabled. Outside a session they
  // would just reset the stream and swallow a half-spoken wake word.
  virtual void SetExitWordsEnabled(bool /*enabled*/) {}
  virtual void SetExitWordsEnabled(bool strong,bool goodbye) { SetExitWordsEnabled(strong || goodbye); }
};

}  // namespace xiaoai_plus::wakeup
