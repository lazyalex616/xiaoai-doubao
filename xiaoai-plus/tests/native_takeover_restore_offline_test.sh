#!/bin/sh
# Runs the real restore_steps control flow against private fake sockets/mounts.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NATIVE_TAKEOVER_LIBRARY_ONLY=1
export NATIVE_TAKEOVER_LIBRARY_ONLY
. "$ROOT/toolchain/native-takeover.sh"
TEST_DIR=$(mktemp -d /tmp/xiaoai-restore-XXXXXX)
trap 'rm -rf "$TEST_DIR"' EXIT
bounded() {
  if [ "${MOCK_TIMEOUT_COMMAND:-}" = "$1:$2" ]; then
    printf '%s\n' timeout >>"$MOCK_EVENTS"
    return 137
  fi
  "$@"
}

mounted() {
  if [ "$1" = "$SPEECH" ]; then [ -f "$STATE/mount.speech" ]; return; fi
  if [ "$1" = "$PNS_INIT" ]; then [ -f "$STATE/mount.pns" ]; return; fi
  return 1
}
umount() {
  if [ "$1" = "$SPEECH" ]; then
    printf '%s\n' umount_speech >>"$MOCK_EVENTS"
    rm "$STATE/mount.speech"
  elif [ "$1" = "$PNS_INIT" ]; then
    printf '%s\n' umount_pns >>"$MOCK_EVENTS"
    [ "${MOCK_FAIL_PNS_UMOUNT:-0}" = 0 ] || return 1
    rm "$STATE/mount.pns"
  else return 1; fi
}
stop_named() {
  if [ "$1" = "$PNS_INIT" ]; then printf '%s\n' stop_pns >>"$MOCK_EVENTS"
  else printf '%s\n' stop_mico >>"$MOCK_EVENTS"; fi
}
stop_probe() { printf '%s\n' stop_probe >>"$MOCK_EVENTS"; }
native_readback() {
  printf '%s\n' readback >>"$MOCK_EVENTS"
  [ ! -e "$STATE/mount.speech" ] && [ ! -e "$STATE/mount.pns" ] &&
    [ ! -e "$NATIVE_SPEECH" ] && [ -S "$SPEECH" ] &&
    [ -f "$STATE/mico.started" ] && [ -f "$STATE/pns.started" ]
}
socket_file() {
  python3 - "$1" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(sys.argv[1])
s.close()
PY
}
prepare_case() {
  name=$1
  STATE=$TEST_DIR/$name/state
  SPEECH_DIR=$TEST_DIR/$name/usock
  SPEECH=$SPEECH_DIR/speech.usock
  NATIVE_SPEECH=$SPEECH_DIR/speech.native.usock
  PNS_INIT=$TEST_DIR/$name/pns-init
  MICO_INIT=$TEST_DIR/$name/mico-init
  MOCK_EVENTS=$TEST_DIR/$name/events
  export STATE MOCK_EVENTS
  mkdir -m 700 -p "$STATE" "$SPEECH_DIR"
  printf '%s\n' ARMED >"$STATE/armed"
  printf '%s\n' 999999 >"$STATE/launcher.pid"
  printf '%s\n' 1 >"$STATE/launcher.birth"
  : >"$STATE/mount.speech"
  : >"$STATE/mount.pns"
  socket_file "$SPEECH"
  socket_file "$NATIVE_SPEECH"
  cat >"$MICO_INIT" <<'SH'
#!/bin/sh
printf '%s\n' start_mico >>"$MOCK_EVENTS"
touch "$STATE/mico.started"
SH
  cat >"$PNS_INIT" <<'SH'
#!/bin/sh
printf '%s\n' start_pns >>"$MOCK_EVENTS"
touch "$STATE/pns.started"
SH
  chmod 700 "$MICO_INIT" "$PNS_INIT"
}

prepare_case happy
restore >"$TEST_DIR/happy/log" 2>&1
[ "$(cat "$STATE/result")" = RESTORE_CONFIRMED ]
[ "$(tr '\n' ' ' <"$MOCK_EVENTS")" = \
  'stop_pns stop_mico stop_probe umount_speech umount_pns start_mico start_pns readback ' ]

# A full diagnostic log cannot prevent the real restore flow from removing
# mounts and verifying original services. /dev/full is only inside this fake
# fixture; no device paths are touched.
prepare_case log_full
ln -s /dev/full "$STATE/steps.log"
restore >"$TEST_DIR/log_full/log" 2>&1
[ "$(cat "$STATE/result")" = RESTORE_CONFIRMED ]
[ ! -e "$STATE/mount.speech" ] && [ ! -e "$STATE/mount.pns" ]
grep -q 'WARN diagnostic step receipt' "$TEST_DIR/log_full/log"

prepare_case failed_unmount
MOCK_FAIL_PNS_UMOUNT=1
if restore >"$TEST_DIR/failed_unmount/log" 2>&1; then exit 1; fi
[ "$(cat "$STATE/result")" = RESTORE_UNVERIFIED ]
[ -f "$STATE/mount.pns" ]
[ ! -f "$STATE/mico.started" ]
before=$(wc -l <"$MOCK_EVENTS")
if restore >/dev/null 2>&1; then exit 1; fi
[ "$(wc -l <"$MOCK_EVENTS")" -eq "$before" ]

# A timed-out init start can leave a partial service state. It must seal the
# attempt as unverified and never report success from the earlier mico start.
prepare_case timed_out_pns_start
MOCK_FAIL_PNS_UMOUNT=0
MOCK_TIMEOUT_COMMAND="$PNS_INIT:start"
if restore >"$TEST_DIR/timed_out_pns_start/log" 2>&1; then exit 1; fi
[ "$(cat "$STATE/result")" = RESTORE_UNVERIFIED ]
[ -f "$STATE/mico.started" ]
[ ! -f "$STATE/pns.started" ]
grep -q '^timeout$' "$MOCK_EVENTS"
before=$(wc -l <"$MOCK_EVENTS")
if restore >/dev/null 2>&1; then exit 1; fi
[ "$(wc -l <"$MOCK_EVENTS")" -eq "$before" ]
printf '%s\n' 'native takeover fake mount/service restore PASS'
