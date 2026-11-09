#!/bin/sh
# Models launcher/guard ownership and slow recovery with only local files.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT=$ROOT/toolchain/native-takeover.sh
FIXTURE=$ROOT/tests/native_takeover_guard_fixture.sh
TEST_DIR=$(mktemp -d /tmp/xiaoai-guard-XXXXXX)
trap 'rm -rf "$TEST_DIR"' EXIT

run_case() {
  case_name=$1 launcher_delay=$2 guard_max=$3 guard_grace=$4
  case_kind=${5:-mutated}
  expected_result=${6:-RESTORE_CONFIRMED}
  case_dir=$TEST_DIR/$case_name
  mkdir -m 700 "$case_dir"
  mkdir -m 700 "$case_dir/state"
  printf '%s\n' ARMED >"$case_dir/state/armed"
  role=launcher
  if [ "$case_kind" = mutated ]; then
    printf '%s\n' STARTED >"$case_dir/state/mutation_started"
  elif [ "$case_kind" = delayed_restore ]; then
    printf '%s\n' STARTED >"$case_dir/state/mutation_started"
    role=delayed_restore
  else
    role=pre_mutation
  fi
  sh "$FIXTURE" "$SCRIPT" "$case_dir" "$role" "$launcher_delay" "${7:-0}" 0 \
    >"$case_dir/launcher.log" 2>&1 &
  launcher_pid=$!
  i=0
  while [ "$i" -lt 100 ] && [ ! -s "$case_dir/events" ]; do sleep 0.02; i=$((i+1)); done
  [ -s "$case_dir/events" ]
  sh "$FIXTURE" "$SCRIPT" "$case_dir" guard 0 "$guard_max" "$guard_grace" \
    >"$case_dir/guard.log" 2>&1 &
  guard_pid=$!
  if wait "$launcher_pid"; then launcher_rc=0; else launcher_rc=$?; fi
  if wait "$guard_pid"; then guard_rc=0; else guard_rc=$?; fi
  [ "$guard_rc" -eq 0 ]
  [ "$(cat "$case_dir/state/result")" = "$expected_result" ]
  [ ! -L "$case_dir/state/restore.lock" ]
}

# A legitimate four-second restore finishes before the guard's six-second
# deadline. The guard sees its readback and never starts another restore.
run_case normal 4 6 3
[ "$launcher_rc" -eq 0 ]
grep -q '^launcher_END$' "$TEST_DIR/normal/events"
if grep -q '^guard_START$' "$TEST_DIR/normal/events"; then exit 1; fi

# The probe deadline fires while the launcher holds restore.lock. The guard
# must not TERM that sole restorer; it waits for the separate restore grace.
run_case deadline_during_restore 4 1 6
[ "$launcher_rc" -eq 0 ]
grep -q '^launcher_END$' "$TEST_DIR/deadline_during_restore/events"
if grep -q '^guard_START$' "$TEST_DIR/deadline_during_restore/events"; then exit 1; fi
grep -q 'probe deadline reached' "$TEST_DIR/deadline_during_restore/guard.log"

# At the first guard deadline there is no restore.lock. The launcher claims
# it one second later, while the guard is in its passive completion window.
# No check-then-TERM path may interrupt this new sole restorer.
run_case delayed_claim 2 1 6 delayed_restore RESTORE_CONFIRMED 3
[ "$launcher_rc" -eq 0 ]
grep -q '^delayed_restore_END$' "$TEST_DIR/delayed_claim/events"
if grep -q '^guard_START$' "$TEST_DIR/delayed_claim/events"; then exit 1; fi

# A hung launcher is killed after the shortened mock budget. The old lock's
# PID/birth is stale before the guard claims recovery; no overlap is accepted.
run_case killed 8 1 2
[ "$launcher_rc" -ne 0 ]
grep -q '^guard_END$' "$TEST_DIR/killed/events"
if grep -q '^launcher_END$' "$TEST_DIR/killed/events"; then exit 1; fi

# A stalled launcher must be stopped before publishing NO_MUTATION. Otherwise
# it can resume takeover after the guard has exited.
run_case pre_mutation_stall 8 1 2 pre_mutation NO_MUTATION
[ "$launcher_rc" -ne 0 ]
if grep -q '^pre_mutation_END$' "$TEST_DIR/pre_mutation_stall/events"; then exit 1; fi
printf '%s\n' 'native takeover independent guard mock PASS'
