#!/bin/sh
# Fake PNS/procd timing only: no service, mount, socket, device or network.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NATIVE_TAKEOVER_LIBRARY_ONLY=1
export NATIVE_TAKEOVER_LIBRARY_ONLY
. "$ROOT/toolchain/native-takeover.sh"
TEST_DIR=$(mktemp -d /tmp/xiaoai-native-stop-XXXXXX)
trap 'rm -rf "$TEST_DIR"' EXIT
STATE=$TEST_DIR/state
PNS_INIT=$TEST_DIR/pns-init
PNS_NAMES=mipns-xiaomi
mkdir -m 700 "$STATE"
printf '%s\n' 0 >"$TEST_DIR/tick"
bounded() { return "$MOCK_INIT_RC"; }
sleep() { tick=$(cat "$TEST_DIR/tick"); printf '%s\n' "$((tick+1))" >"$TEST_DIR/tick"; }
pidof() {
  [ "$1" = mipns-xiaomi ] || return 1
  tick=$(cat "$TEST_DIR/tick")
  case "$MOCK_CASE" in
    delayed) [ "$tick" -lt 7 ] && { printf '%s\n' 1403; return 0; };;
    failed_init) [ "$tick" -lt 3 ] && { printf '%s\n' 1403; return 0; };;
    respawn) [ $((tick % 2)) -eq 0 ] && { printf '%s\n' 1403; return 0; };;
    late_respawn) { [ "$tick" -lt 2 ] || [ "$tick" -ge 7 ]; } && {
      printf '%s\n' 1403; return 0;
    };;
  esac
  return 1
}

MOCK_CASE=delayed MOCK_INIT_RC=0
stop_named "$PNS_INIT" "$PNS_NAMES" TEST_DELAYED
[ "$(cat "$TEST_DIR/tick")" -ge 13 ]
grep -q 'stage=TEST_DELAYED.STOP_STABLE rc=0' "$STATE/steps.log"

printf '%s\n' 0 >"$TEST_DIR/tick"
MOCK_CASE=failed_init MOCK_INIT_RC=7
stop_named "$PNS_INIT" "$PNS_NAMES" TEST_NONZERO_RC
grep -q 'stage=TEST_NONZERO_RC.STOP_STABLE rc=7' "$STATE/steps.log"

printf '%s\n' 0 >"$TEST_DIR/tick"
MOCK_CASE=respawn MOCK_INIT_RC=0
if stop_named "$PNS_INIT" "$PNS_NAMES" TEST_RESPAWN; then
  echo 'respawn was accepted as stopped' >&2
  exit 1
fi
grep -q 'stage=TEST_RESPAWN.STOP_TIMEOUT' "$STATE/steps.log"

# A procd child can disappear briefly and respawn five seconds later. A
# two-sample zero check would pass too early; the longer stability window must
# reset when it returns.
printf '%s\n' 0 >"$TEST_DIR/tick"
MOCK_CASE=late_respawn MOCK_INIT_RC=0
if stop_named "$PNS_INIT" "$PNS_NAMES" TEST_LATE_RESPAWN; then
  echo 'late procd respawn was accepted as stopped' >&2
  exit 1
fi
grep -q 'stage=TEST_LATE_RESPAWN.STOP_TIMEOUT' "$STATE/steps.log"

# Frontend process presence alone is insufficient; it must be stable in PCM
# mode for two observations before a fast-exiting probe can trigger recovery.
same_process() { [ "$1" = guard ]; }
native_processes() { tick=$(cat "$TEST_DIR/tick"); [ "$tick" -ge 2 ]; }
pns_is_pcm() { tick=$(cat "$TEST_DIR/tick"); [ "$tick" -ge 4 ]; }
printf '%s\n' 0 >"$TEST_DIR/tick"
wait_pcm_frontend
[ "$(cat "$TEST_DIR/tick")" -ge 5 ]
grep -q 'stage=PCM_FRONTEND_STABLE' "$STATE/steps.log"
# Two bounded init stops + stable waits, probe stop, two umounts, two init
# starts and the 20-second native readback must fit beneath guard grace.
worst=$((2*(COMMAND_TIMEOUT_SECONDS+STOP_WAIT_SECONDS)+5+2*COMMAND_TIMEOUT_SECONDS+2*COMMAND_TIMEOUT_SECONDS+20+10))
[ "$RESTORE_GRACE_SECONDS" -gt "$worst" ]
printf '%s\n' 'native takeover delayed stop/respawn/PCM stability PASS'
