"""Exercise the startup guardian with a disposable crashing fake speaker."""

from pathlib import Path
import os
import signal
import subprocess
import tempfile
import time


SOURCE = Path(__file__).resolve().parents[1] / "boot.sh"


def test_guardian() -> None:
    with tempfile.TemporaryDirectory(prefix="xiaoai-guardian-") as directory:
        root = Path(directory)
        (root / "assets").mkdir()
        (root / "assets/ca-certificates.crt").write_text("fake offline certificate\n")
        (root / "config.ini").write_text("fake offline config\n")
        speaker = root / "xiaoai_plus_speaker"
        speaker.write_text("#!/bin/sh\n"
                           "[ \"$1\" = --check-config ] && exit 0\n"
                           "echo attempt >> \"${0%/*}/attempts\"\n"
                           "exit 17\n")
        speaker.chmod(0o700)
        script = root / "boot.sh"
        script.write_text(SOURCE.read_text().replace("/data/xiaoai-plus", directory)
                          .replace("WAIT_SECONDS=60", "WAIT_SECONDS=0"))
        script.chmod(0o600)
        result = subprocess.run(["/bin/sh", str(script)], capture_output=True,
                                text=True, timeout=5, check=False)
        assert result.returncode == 0, result.stderr
        log = root / "xiaoai_plus.log"
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if log.exists() and "stopped after five failures in 30 minutes" in log.read_text():
                break
            time.sleep(0.1)
        assert "stopped after five failures in 30 minutes" in log.read_text()
        assert (root / "attempts").read_text().splitlines() == ["attempt"] * 5
        assert not (root / "guardian.pid").exists()


def test_log_bound_while_running() -> None:
    with tempfile.TemporaryDirectory(prefix="xiaoai-guardian-log-") as directory:
        root = Path(directory)
        (root / "assets").mkdir()
        (root / "assets/ca-certificates.crt").write_text("offline certificate\n")
        (root / "config.ini").write_text("offline config\n")
        speaker = root / "xiaoai_plus_speaker"
        speaker.write_text("#!/bin/sh\n"
                           "[ \"$1\" = --check-config ] && exit 0\n"
                           "echo $$ > \"${0%/*}/speaker.pid\"\n"
                           "head -c 2097152 /dev/zero | tr '\\000' x\n"
                           "echo archive-marker\n"
                           "sleep 3\n"
                           "echo descriptor-still-live\n"
                           "sleep 30\n")
        speaker.chmod(0o700)
        script = root / "boot.sh"
        script.write_text(SOURCE.read_text().replace("/data/xiaoai-plus", directory)
                          .replace("WAIT_SECONDS=60", "WAIT_SECONDS=0")
                          .replace("4194304", "1048576"))
        subprocess.run(["/bin/sh", str(script)], capture_output=True,
                       text=True, timeout=5, check=True)
        try:
            deadline = time.monotonic() + 10
            while not (root / "speaker.pid").exists() and time.monotonic() < deadline:
                time.sleep(0.1)
            assert (root / "speaker.pid").exists()
            time.sleep(5)
            log, previous = root / "xiaoai_plus.log", root / "xiaoai_plus.log.1"
            assert "archive-marker" in previous.read_text()
            assert "descriptor-still-live" in log.read_text()
            assert "guardian: log rotated" in log.read_text()
            assert log.stat().st_size <= 1048576
            assert previous.stat().st_size == 1048576
        finally:
            for name in ("guardian.pid", "speaker.pid"):
                if (root / name).exists():
                    os.kill(int((root / name).read_text()), signal.SIGTERM)


def test_failures_accumulate_after_long_runs() -> None:
    with tempfile.TemporaryDirectory(prefix="xiaoai-guardian-window-") as directory:
        root = Path(directory)
        (root / "assets").mkdir()
        (root / "assets/ca-certificates.crt").write_text("offline\n")
        (root / "config.ini").write_text("offline\n")
        speaker = root / "xiaoai_plus_speaker"
        speaker.write_text("#!/bin/sh\n"
                           "[ \"$1\" = --check-config ] && exit 0\n"
                           "echo attempt >> \"${0%/*}/attempts\"\n"
                           "exit 17\n")
        speaker.chmod(0o700)
        date = root / "date"
        date.write_text("#!/bin/sh\n"
                        f"n=$(cat '{root}/clock' 2>/dev/null || echo 0)\n"
                        f"echo $((n + 1)) > '{root}/clock'\n"
                        "if [ $((n % 2)) -eq 0 ]; then echo $((1000 + n / 2 * 5)); "
                        "else echo $((1301 + n / 2 * 5)); fi\n")
        date.chmod(0o700)
        script = root / "boot.sh"
        script.write_text(SOURCE.read_text().replace("/data/xiaoai-plus", directory)
                          .replace("WAIT_SECONDS=60", "WAIT_SECONDS=0"))
        env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}")
        subprocess.run(["/bin/sh", str(script)], env=env, capture_output=True,
                       text=True, timeout=5, check=True)
        log = root / "xiaoai_plus.log"
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline:
            if log.exists() and "stopped after five failures in 30 minutes" in log.read_text():
                break
            time.sleep(0.1)
        assert "stopped after five failures in 30 minutes" in log.read_text()
        assert len((root / "attempts").read_text().splitlines()) == 5


if __name__ == "__main__":
    test_guardian()
    test_log_bound_while_running()
    test_failures_accumulate_after_long_runs()
