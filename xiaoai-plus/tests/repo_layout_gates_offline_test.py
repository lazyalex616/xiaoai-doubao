"""Run consumed P4c/P4d host entries; reject before any device utility or write."""
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
manifest = ROOT / "toolchain/MANIFEST.env"
before = manifest.read_bytes()
assert b"ALLOW_P4C_ELF_UPDATE=0\n" in before
assert b"ALLOW_P4D_ELF_UPDATE=0\n" in before
with tempfile.TemporaryDirectory(prefix="repo-layout-gates-") as temporary:
    shim = Path(temporary)
    marker = shim / "unexpected-device-command"
    for name in ("ssh", "scp", "sftp", "arp", "ssh-keygen"):
        path = shim / name
        path.write_text(f'#!/bin/sh\necho {name} >> "{marker}"\nexit 99\n')
        path.chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = str(shim) + os.pathsep + env["PATH"]
    for phase in ("p4c", "p4d"):
        result = subprocess.run(
            ["bash", str(ROOT / f"toolchain/{phase}-install.sh")],
            env=env, capture_output=True, timeout=10,
        )
        assert result.returncode == 1, (phase, result.returncode)
        assert b"requires Bash 4+" in result.stderr or b"installation permit closed" in result.stderr
        assert not marker.exists(), "device command attempted"
        assert manifest.read_bytes() == before, "MANIFEST changed"
        print(f"{phase}: rejected before device utilities; MANIFEST unchanged")
