"""Run the real readback body with fake project files and failed sub-checks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'toolchain/p4c-install-remote.sh').read_text()
body = source.split('readback() {\n', 1)[1].split('\n}\nrollback()', 1)[0]
with tempfile.TemporaryDirectory(prefix='p4c-readback-') as tmp:
    app = Path(tmp)
    for name in ('elf', 'boot', 'words', 'config.ini'):
        (app/name).write_text(name)
    (app/'boot').chmod(0o755)
    # sha256sum shim supports macOS too; no stat executable is supplied.
    shim = app/'tools'
    shim.mkdir()
    (shim/'sha256sum').write_text('#!/bin/sh\nexec shasum -a 256 "$@"\n')
    (shim/'sha256sum').chmod(0o755)
    script = '''set -eu
app=$1; bin=$app/elf; boot=$app/boot; words=$app/words
hash() { sha256sum "$1" | cut -d ' ' -f1; }
config_hash=$(hash "$app/config.ini")
KWS_REPEAT_END_SHA256=$(hash "$words")
validate_running() { gp=1; sp=2; rec=3; [ "$case_name" != bad_identity ]; }
native_snapshot() { [ "$case_name" != bad_native ]; }
case_name=$2
case "$case_name" in bad_mode) chmod 600 "$boot";; bad_config) echo changed >> "$app/config.ini";; esac
readback() {
''' + body + '\n}\nreadback\n'
    for case in ('ok', 'bad_mode', 'bad_identity', 'bad_native', 'bad_config'):
        (app/'boot').chmod(0o755)
        (app/'config.ini').write_text('config.ini')
        result = subprocess.run(['sh', '-c', script, 'fixture', str(app), case],
                                env=dict(os.environ, PATH=str(shim)+os.pathsep+os.environ['PATH']),
                                capture_output=True, text=True)
        assert (result.returncode == 0) == (case == 'ok'), (case, result.returncode, result.stdout, result.stderr)
print('P4c POSIX readback and mode/config/identity/native failure checks PASS')
