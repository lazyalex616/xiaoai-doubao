#!/usr/bin/env python3
"""No-device fault injection: independent guard survives launcher session HUP."""

import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "toolchain/native-takeover.sh"
FIXTURE = ROOT / "tests/native_takeover_guard_fixture.sh"
HELPER = Path(os.environ["NATIVE_SETSID_HELPER"])


def wait_for(path: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while not path.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert path.exists(), f"missing {path.name}"


def wait_for_clean_result(state: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = state / "result"
        if result.exists() and not (state / "restore.lock").is_symlink():
            return
        time.sleep(0.02)
    raise AssertionError("independent guard did not finish a clean restore")


with tempfile.TemporaryDirectory(prefix="xiaoai-native-detach-") as tmp:
    case = Path(tmp)
    state = case / "state"
    state.mkdir(mode=0o700)
    (state / "armed").write_text("ARMED\n")
    (state / "mutation_started").write_text("STARTED\n")
    launcher_log = (case / "launcher.log").open("wb")
    guard_log = (case / "guard.log").open("wb")
    launcher = subprocess.Popen(
        ["sh", str(FIXTURE), str(SCRIPT), str(case), "launcher", "8", "0", "0"],
        stdin=subprocess.DEVNULL,
        stdout=launcher_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    try:
        wait_for(case / "events", 2)
        guard = subprocess.Popen(
            [str(HELPER), "/bin/sh", str(FIXTURE), str(SCRIPT), str(case), "guard", "0", "8", "2"],
            stdin=subprocess.DEVNULL,
            stdout=guard_log,
            stderr=subprocess.STDOUT,
            start_new_session=False,
        )
        try:
            assert guard.wait(timeout=2) == 0
            wait_for(state / "guard.ready", 2)
            guard_child_pid = int((state / "guard.pid").read_text())
            time.sleep(0.2)
            assert os.getsid(guard_child_pid) == guard_child_pid
            assert os.getsid(launcher.pid) != os.getsid(guard_child_pid)
            os.killpg(launcher.pid, signal.SIGHUP)
            assert launcher.wait(timeout=3) != 0
            wait_for_clean_result(state, 12)
            assert (state / "result").read_text().strip() == "RESTORE_CONFIRMED"
            assert (case / "native_restored").is_file()
            assert not (state / "restore.lock").is_symlink()
            events = (case / "events").read_text().splitlines()
            assert "guard_END" in events and "launcher_END" not in events
        finally:
            if guard.poll() is None:
                guard.kill()
                guard.wait(timeout=3)
    finally:
        if launcher.poll() is None:
            launcher.kill()
            launcher.wait(timeout=3)
        launcher_log.close()
        guard_log.close()

print("native takeover detached guard HUP mock PASS")
