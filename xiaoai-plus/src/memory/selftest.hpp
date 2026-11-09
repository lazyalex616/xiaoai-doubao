#pragma once
#include <ostream>
#include "memory/service.hpp"
namespace xiaoai_plus::memory {
// Debug CLI only: Service + synthetic text, no App/recorder/player/realtime Client.
int Selftest(config::Memory cfg, Transport transport, std::ostream& out,
             int summary_wait_seconds = 45, int delete_wait_seconds = 120);
}
