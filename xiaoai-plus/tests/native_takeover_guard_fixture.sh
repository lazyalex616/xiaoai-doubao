#!/bin/sh
# Private fixture for native_takeover_guard_offline_test.sh; no hardware calls.
set -eu
SCRIPT=$1
TEST_DIR=$2
ROLE=$3
DELAY=$4
MAX_WAIT=$5
GRACE=$6
NATIVE_TAKEOVER_LIBRARY_ONLY=1
export NATIVE_TAKEOVER_LIBRARY_ONLY
. "$SCRIPT"
STATE=$TEST_DIR/state
GUARD_MAX_SECONDS=$MAX_WAIT
RESTORE_GRACE_SECONDS=$GRACE
native_readback() { [ -f "$TEST_DIR/native_restored" ]; }
restore_steps() {
  printf '%s_START\n' "$ROLE" >>"$TEST_DIR/events"
  sleep "$DELAY"
  : >"$TEST_DIR/native_restored"
  printf '%s_END\n' "$ROLE" >>"$TEST_DIR/events"
  return 0
}
if [ "$ROLE" = launcher ]; then
  save_process launcher "$$"
  restore
elif [ "$ROLE" = guard ]; then
  guard
elif [ "$ROLE" = pre_mutation ]; then
  save_process launcher "$$"
  printf '%s\n' pre_mutation_START >>"$TEST_DIR/events"
  sleep "$DELAY"
  printf '%s\n' pre_mutation_END >>"$TEST_DIR/events"
elif [ "$ROLE" = delayed_restore ]; then
  save_process launcher "$$"
  printf '%s\n' launcher_PENDING >>"$TEST_DIR/events"
  sleep "$DELAY"
  DELAY=$MAX_WAIT
  restore
else
  exit 2
fi
