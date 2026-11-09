#ifdef NDEBUG
#error assertions required
#endif
#include <cassert>
#include <chrono>
#include <iostream>
#include "wakeup/local_listener.hpp"
using namespace std::chrono_literals;
int main(){using L=xiaoai_plus::wakeup::LocalListener;const auto due=std::chrono::steady_clock::time_point(10s);
 L::Decision p{"豆包",false,due-1100ms,true,due};
 for(auto delta:{0ms,1ms,500ms})assert(p.CanCommitAt(due+delta,due+delta-10ms));
 assert(!p.CanCommitAt(due+501ms,due+501ms-10ms));
 assert(!p.CanCommitAt(due+1ms,due+1ms-501ms));
 assert(!p.CanCommitAt(due+501ms,due+501ms-10ms)); // Repeated calls do not extend fixed due.
 L::Decision hit{"豆包豆包",false,due-500ms,false,{}};assert(hit.CanCommitAt(due,due-500ms));
 assert(!hit.CanCommitAt(due+1ms,due-500ms));
 std::cout<<"PASS prefix fixed due exact/+1/+500 accepted,+501 refused; fresh Tick/hit separate <=500ms\n";
}
