"""Exercise the actual rollback trap after actual CPU/overrun guard failures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'toolchain/p4d-install-remote.sh').read_text()
rollback = source.split('rollback() {\n', 1)[1].split('\n}\ntrap rollback EXIT', 1)[0]
guards = source.split('metrics_finish\n', 1)[1].split('\nreadback\n', 1)[0]
for reason in ('overrun', 'cpu', 'readback'):
    with tempfile.TemporaryDirectory(prefix='p4d-rollback-') as tmp:
        app = Path(tmp); rb = app/'rollback'; rb.mkdir()
        old = '#!/bin/sh\nexit 0\n# old ELF\n'
        (rb/'xiaoai_plus_speaker').write_text(old)
        (rb/'xiaoai_plus_speaker').chmod(0o755)
        (rb/'boot.sh').write_text('old boot\n'); (rb/'boot.sh').chmod(0o755)
        (app/'elf').write_text('candidate ELF\n'); (app/'boot').write_text('candidate boot\n')
        script = '''set -eu
app=$1; rb=$app/rollback; bin=$app/elf; boot=$app/boot
hash() { shasum -a 256 "$1" | cut -d ' ' -f1; }
INSTALLED_ELF_SHA256=$(hash "$rb/xiaoai_plus_speaker")
stop_project() { echo fixture_stopped; }
start_ready() { [ "$(hash "$bin")" = "$INSTALLED_ELF_SHA256" ] && [ "$require_agc" = 0 ]; }
readback() { [ "$2" != readback ]; }
''' .replace('readback() { [ "$2" != readback ]; }',
             'readback() { [ "'+reason+'" != readback ]; }')
        script += 'rollback() {\n'+rollback+'\n}\ntrap rollback EXIT\n'
        script += f'''P4D_BASELINE_OVERRUNS=10; P4D_BASELINE_SECONDS=1800
P4D_BASELINE_CPU=250; elapsed=1800; overrun_delta={16 if reason != 'cpu' else 10}; cpu={276 if reason == 'cpu' else 250}
''' + guards
        result = subprocess.run(['sh', '-c', script, 'fixture', tmp], capture_output=True, text=True)
        assert result.returncode != 0, result
        assert (app/'elf').read_text() == old, result.stderr
        assert (app/'boot').read_text() == 'old boot\n', result.stderr
        assert (app/'boot').stat().st_mode & 0o777 == 0o755
        expected = 'P4D_RESTORE_UNVERIFIED' if reason == 'readback' else 'P4D_ROLLBACK_VERIFIED'
        assert expected in result.stderr, result.stderr
print('P4d real rollback trap: CPU/overrun restore + readback failure remains unverified PASS')
