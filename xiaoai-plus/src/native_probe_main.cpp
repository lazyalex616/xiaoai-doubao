#include <chrono>
#include <iostream>
#include <string>

#include "native_speech/probe.hpp"

int main(int argc, char** argv) {
  int seconds = 30;
  bool self_test = false;
  bool expect_turn = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg == "--seconds" && i + 1 < argc) {
      try { seconds = std::stoi(argv[++i]); }
      catch (...) { return 2; }
    } else if (arg == "--self-test") {
      self_test = true;
    } else if (arg == "--expect-turn") {
      expect_turn = true;
    } else {
      return 2;
    }
  }
  if (seconds < 1 || seconds > 30) return 2;
  if (self_test) {
    if (argc != 2) return 2;
    std::cout << "native no-cloud probe self-test ok" << std::endl;
    return 0;
  }
  xiaoai_plus::native_speech::Probe::Counts counts;
  xiaoai_plus::native_speech::Probe probe;
  const bool okay = probe.Run(std::chrono::seconds(seconds), &counts, expect_turn);
  const bool wake_seen = counts.wakes > 0;
  const bool pcm_valid = counts.pcm_packets > 0 && counts.pcm_bytes > 0;
  const bool turn_end = counts.ended > 0;
  const bool complete_turn = counts.complete_turns > 0;
  std::cout << "native_probe check_pass=" << (okay ? 1 : 0)
            << " requested=" << (expect_turn ? "COMPLETE_TURN" : "REGISTER_ONLY")
            << " observed=" << (complete_turn ? "COMPLETE_TURN" :
                turn_end ? "TURN_END" : pcm_valid ? "PCM_VALID" :
                wake_seen ? "WAKE_SEEN" : counts.registered ? "REGISTER_ONLY" : "UNREGISTERED")
            << " registered=" << (counts.registered ? 1 : 0)
            << " wake_seen=" << (wake_seen ? 1 : 0)
            << " pcm_valid=" << (pcm_valid ? 1 : 0)
            << " turn_end=" << (turn_end ? 1 : 0)
            << " complete_turn=" << (complete_turn ? 1 : 0)
            << " followup_validated=0"
            << " wakes=" << counts.wakes
            << " followups=" << counts.followups
            << " pcm_packets=" << counts.pcm_packets
            << " pcm_bytes=" << counts.pcm_bytes
            << " ended=" << counts.ended
            << " reg_stage=" << xiaoai_plus::native_speech::Source::RegistrationStageName(counts.registration.stage)
            << " fatal_class=" << xiaoai_plus::native_speech::Source::FatalClassName(counts.registration.fatal_class)
            << " fatal_count=" << counts.registration.fatal_count
            << " datagrams_seen=" << counts.registration.datagrams_seen
            << " peer_path_rejected=" << counts.registration.peer_path_rejected
            << " peer_owner_rejected=" << counts.registration.peer_owner_rejected
            << " peer_qualified=" << counts.registration.peer_qualified
            << " register_packets_seen=" << counts.registration.register_packets_seen
            << " codec_absent=" << counts.registration.codec_absent
            << " codec_empty=" << counts.registration.codec_empty
            << " codec_explicit_pcm=" << counts.registration.codec_explicit_pcm
            << " codec_explicit_rejected=" << counts.registration.codec_explicit_rejected
            << " register_ready=" << counts.registration.register_ready
            << std::endl;
  return okay ? 0 : 1;
}
