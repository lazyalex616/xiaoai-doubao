#include <cassert>
#include <iostream>
#include "realtime/barge_in_controller.hpp"
using C = xiaoai_plus::realtime::BargeInController;
int main() {
  const C::Eligibility yes{true,true,false,false,true};
  C off; off.BeginSession(42, 1); assert(!off.active());
  assert(off.ResponseCreated("r1") == C::Created::kRejected);
  assert(!off.ConfirmSpeech(off.token(),yes));
  std::cout << "PASS default_off\n";
  C c(true); c.BeginSession(42,1);
  assert(c.ResponseCreated("") == C::Created::kRejected);
  assert(c.ResponseCreated("r1") == C::Created::kAccepted);
  assert(c.ResponseCreated("r1") == C::Created::kDuplicate);
  assert(c.Audio("r1")); const auto before = c.token();
  for (int i=0;i<5;++i) {
    auto no=yes;
    if(i==0) no.active=false; if(i==1) no.ready=false;
    if(i==2) no.identity_switching=true; if(i==3) no.native_blocked=true;
    if(i==4) no.capture_continuous=false;
    assert(!c.ConfirmSpeech(before,no)); assert(c.token()==before);
  }
  std::cout << "PASS eligibility_identity_switch_native_discontinuity\n";
  auto stale=before; --stale.epoch; assert(!c.ConfirmSpeech(stale,yes));
  stale=before; --stale.response; assert(!c.ConfirmSpeech(stale,yes));
  stale=before; --stale.playback; assert(!c.ConfirmSpeech(stale,yes));
  stale=before; --stale.ingress; assert(!c.ConfirmSpeech(stale,yes));
  stale=before; --stale.capture; assert(!c.ConfirmSpeech(stale,yes));
  assert(c.ConfirmSpeech(before,yes)); const auto fresh=c.token();
  assert(fresh.epoch==before.epoch && fresh.session==before.session);
  assert(fresh.ingress==before.ingress+1 && fresh.playback==before.playback+1);
  assert(!c.ConfirmSpeech(before,yes) && !c.ConfirmSpeech(fresh,yes));
  assert(!c.AcceptInput(before) && !c.AcceptInput(fresh));
  assert(!c.AuthorizeNextResponse(fresh,"r2"));
  assert(!c.CompleteCancellation(before));
  assert(c.CompleteCancellation(fresh) && c.AcceptInput(fresh));
  assert(!c.CompleteCancellation(fresh));
  assert(!c.AcceptOutput("r1") && !c.AcceptOutput(""));
  assert(!c.AudioDone("r1"));
  std::cout << "PASS one_shot_cancel_preserves_epoch_and_session_old_ingress_rejected\n";
  assert(c.ResponseCreated("r1")==C::Created::kRejected);
  assert(c.ResponseCreated("unseen-late-created")==C::Created::kRejected);
  assert(c.ResponseCreated("r2")==C::Created::kRejected);
  assert(!c.AuthorizeNextResponse(before,"r2"));
  assert(c.AuthorizeNextResponse(fresh,"r2"));
  assert(c.ResponseCreated("r2")==C::Created::kAccepted);
  assert(!c.AcceptOutput("r1") && !c.AcceptOutput(""));
  assert(c.Audio("r2") && !c.AudioDone("r1")); assert(c.playing());
  assert(c.AudioDone("r2") && c.playing());
  assert(!c.PlaybackDrained(before));
  assert(c.PlaybackDrained(c.token()) && !c.playing());
  assert(!c.ConfirmSpeech(c.token(),yes));
  std::cout << "PASS granted_new_response_only_unseen_late_created_blocked_cloud_done_not_local_drained\n";
  const auto old=c.token(); c.BeginSession(43,2); assert(!c.AcceptInput(old));
  assert(c.ResponseCreated("r2")==C::Created::kAccepted); assert(c.Audio("r2"));
  assert(!c.ConfirmSpeech(old,yes)); c.ChangeCapture(3);
  assert(!c.AcceptInput(c.token()) && !c.ConfirmSpeech(c.token(),yes));
  std::cout << "PASS_cross_session_cross_epoch_capture_invalidation\n";
  C bounded(true); bounded.BeginSession(44,1);
  for(int i=0;i<128;++i) assert(bounded.ResponseCreated("r"+std::to_string(i))==C::Created::kAccepted);
  assert(bounded.ResponseCreated("overflow")==C::Created::kRejected);
  assert(!bounded.active());
  std::cout << "PASS bounded_response_ids_fail_closed\n";
}
