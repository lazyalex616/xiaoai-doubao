#!/usr/bin/env python3
"""Rebuild the OH2P 1.62.2 rootfs with only key-only SSH and a boot hook.

This is an offline builder. It never invokes the Amlogic update tool or reads USB.
The public key is read locally; the private key is never opened.
"""

import argparse
import hashlib
import os
import shutil
import subprocess
from pathlib import Path


BASE = Path(__file__).resolve().parent
SOURCE = BASE.parent / ".local/rootfs-1.62.2"
PACKER = Path(shutil.which("mksquashfs") or "mksquashfs")
EPOCH = 1776396243
EXPECTED = {
    "etc/rc.local": "515018aa94eb4937dade12dc0d69890b352af35a5ab5fb8203f9ed10990ac06f",
    "etc/config/dropbear": "35f340f5019f104bb894354b4033df660a6de6d8cbfb84ada87dea249d881175",
}
RC_LOCAL = """# Local setup: the release build skips Dropbear at S50 without a flag.
# S95done runs rc.local, so set the temporary flag and start it now.
umask 077
if [ -s /etc/dropbear/authorized_keys ]; then
    printf '1\\n' > /tmp/ssh_en
    /etc/init.d/dropbear start
fi

# The runtime is installed later over key-only SSH. Missing files are inert.
if [ -d /data/xiaoai-plus ] && [ ! -L /data/xiaoai-plus ] && \\
   [ -x /data/xiaoai-plus/boot.sh ] && [ ! -L /data/xiaoai-plus/boot.sh ]; then
    /bin/sh /data/xiaoai-plus/boot.sh >/dev/null 2>&1 &
fi

exit 0
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_file(path: Path, data: bytes, mode: int) -> None:
    path.write_bytes(data)
    path.chmod(mode)
    os.utime(path, (EPOCH, EPOCH))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--packer", type=Path, default=PACKER)
    p.add_argument("--public-key", type=Path, required=True)
    p.add_argument("--tree", type=Path, required=True)
    p.add_argument("--image", type=Path, required=True)
    args = p.parse_args()
    tree, image = args.tree.resolve(), args.image.resolve()
    assets = (BASE.parent / ".local").resolve()
    if tree.parent != assets or image.parent != assets or tree.exists() or image.exists():
        p.error("new tree and image paths must be direct children of .local/")
    for rel, expected in EXPECTED.items():
        if sha256(SOURCE / rel) != expected:
            raise SystemExit(f"STOP: original {rel} hash mismatch")
    version = (SOURCE / "usr/share/mico/version").read_text()
    if "OH2P" not in version or "1.62.2" not in version:
        raise SystemExit("STOP: source rootfs identity mismatch")
    public = args.public_key.resolve().read_bytes()
    if not public.startswith(b"ssh-rsa "):
        raise SystemExit("STOP: dedicated public key mismatch")
    if not args.packer.is_file():
        raise SystemExit("STOP: local mksquashfs missing")

    shutil.copytree(SOURCE, tree, symlinks=True, copy_function=shutil.copy2)
    dropbear_config = tree / "etc/config/dropbear"
    config = dropbear_config.read_text()
    for option in ("PasswordAuth", "RootPasswordAuth"):
        old = f"option {option} '1'"
        if config.count(old) != 1:
            raise SystemExit(f"STOP: {option} expected exactly once")
        config = config.replace(old, f"option {option} '0'")
    write_file(dropbear_config, config.encode(), 0o644)
    write_file(tree / "etc/rc.local", RC_LOCAL.encode(), 0o644)
    write_file(tree / "etc/dropbear/authorized_keys", public, 0o600)
    root_ssh = tree / "root/.ssh"
    root_ssh.mkdir(mode=0o700)
    root_ssh.chmod(0o700)
    write_file(root_ssh / "authorized_keys", public, 0o600)
    os.utime(root_ssh, (EPOCH, EPOCH))
    for rel in ("etc", "etc/config", "etc/dropbear", "root"):
        os.utime(tree / rel, (EPOCH, EPOCH))

    cmd = [str(args.packer), str(tree), str(image), "-comp", "xz", "-b", "131072",
           "-all-root", "-no-xattrs", "-repro-time", str(EPOCH), "-noappend",
           "-processors", "2", "-p", "dev/console c 600 0 0 5 1"]
    result = subprocess.run(cmd, check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise SystemExit(f"STOP: mksquashfs exit {result.returncode}: {result.stdout[-2000:]}")
    if image.stat().st_size >= 0x02800000:
        raise SystemExit("STOP: image exceeds upstream OH2P limit")
    print(f"image={image.name} bytes={image.stat().st_size} sha256={sha256(image)}")
    print("modified=etc/rc.local,etc/config/dropbear,etc/dropbear/authorized_keys")
    print("added=root/.ssh,root/.ssh/authorized_keys")


if __name__ == "__main__":
    main()
