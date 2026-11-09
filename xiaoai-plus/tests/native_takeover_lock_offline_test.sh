#!/bin/sh
# Exercises restore ownership only. Never calls init, mount, or a device.
set -eu
SCRIPT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)/toolchain/native-takeover.sh
NATIVE_TAKEOVER_LIBRARY_ONLY=1
export NATIVE_TAKEOVER_LIBRARY_ONLY
. "$SCRIPT"

TEST_DIR=$(mktemp -d /tmp/xiaoai-native-lock-XXXXXX)
trap 'rm -rf "$TEST_DIR"' EXIT
STATE=$TEST_DIR/state
mkdir -m 700 "$STATE"
printf '%s\n' ARMED >"$STATE/armed"
printf '%s\n' 999999 >"$STATE/launcher.pid"
printf '%s\n' 1 >"$STATE/launcher.birth"
native_readback() { return 0; }
restore_steps() {
  printf 'restore\n' >>"$TEST_DIR/calls"
  sleep 1
  return 0
}

restore >"$TEST_DIR/first.log" 2>&1 &
first=$!
sleep 0.1
restore >"$TEST_DIR/second.log" 2>&1 &
second=$!
wait "$first"
if wait "$second"; then
  echo 'concurrent restore was accepted' >&2
  exit 1
fi
[ "$(wc -l <"$TEST_DIR/calls")" -eq 1 ]
[ "$(cat "$STATE/result")" = RESTORE_CONFIRMED ]
[ ! -L "$STATE/restore.lock" ]

# The device version file is UCI; require one exact quoted value for each
# field, rejecting wrong versions and contradictory duplicate options.
VERSION_FILE=$TEST_DIR/version
cat >"$VERSION_FILE" <<'VERSION'
config core 'version'
    option ROM '1.62.2'
    option HARDWARE 'OH2P'
VERSION
version_field_is ROM 1.62.2
version_field_is HARDWARE OH2P
printf "%s\n" "    option HARDWARE 'OTHER'" >>"$VERSION_FILE"
if version_field_is HARDWARE OH2P; then
  echo 'duplicate hardware option was accepted' >&2
  exit 1
fi
printf "%s\n" "    option ROM '1.62.3'" >"$VERSION_FILE"
if version_field_is ROM 1.62.2; then
  echo 'wrong ROM was accepted' >&2
  exit 1
fi

# A failed restore is terminal and cannot be reported as confirmed.
rm "$STATE/result"
restore_steps() {
  printf 'failure\n' >>"$TEST_DIR/calls"
  return 1
}
if restore >"$TEST_DIR/failed.log" 2>&1; then
  echo 'failed restore returned success' >&2
  exit 1
fi
[ "$(cat "$STATE/result")" = RESTORE_UNVERIFIED ]
if restore >/dev/null 2>&1; then
  echo 'unverified restore was retried automatically' >&2
  exit 1
fi
[ "$(wc -l <"$TEST_DIR/calls")" -eq 2 ]

# A successful service readback is not a successful restore unless the result
# receipt can be persisted. Simulate a failed atomic rename.
rm "$STATE/result"
restore_steps() { return 0; }
mv() { return 1; }
if restore >"$TEST_DIR/receipt_failure.log" 2>&1; then
  echo 'failed receipt write returned success' >&2
  exit 1
fi
[ ! -f "$STATE/result" ]
[ ! -L "$STATE/restore.lock" ]
if result_ok; then
  echo 'missing receipt was accepted' >&2
  exit 1
fi
printf '%s\n' UNKNOWN_RESULT >"$STATE/result"
if result_ok; then
  echo 'unknown receipt was accepted' >&2
  exit 1
fi

# Crash after verified receipt but before lock release: reclaim only a dead
# owner, then require a clean lock state for confirmation.
printf '%s\n' RESTORE_CONFIRMED >"$STATE/result"
ln -s 999998:1 "$STATE/restore.lock"
result_ok
[ ! -L "$STATE/restore.lock" ]

# BusyBox command-field matching must not confuse native mediaplayer with aplay,
# and must detect long executable basenames that pidof may truncate.
printf '%s\n' 'PID USER VSZ STAT COMMAND' \
  '1681 root 1234 S /usr/bin/mediaplayer' >"$TEST_DIR/ps"
ps() { cat "$TEST_DIR/ps"; }
no_assistant_processes
printf '%s\n' '2477 root 2345 S /data/xiaoai-plus/xiaoai_native_speaker' >>"$TEST_DIR/ps"
if no_assistant_processes; then
  echo 'long assistant process name was missed' >&2
  exit 1
fi
printf '%s\n' 'native takeover restore ownership mock PASS'
