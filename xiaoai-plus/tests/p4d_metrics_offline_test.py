"""Exercise actual P4d CPU/rate guards at limits and resource regressions."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'toolchain/p4d-install-remote.sh').read_text()
guards = source.split('metrics_finish\n', 1)[1].split('\nreadback\n', 1)[0]
cases = [
    # baseline_count, baseline_seconds, candidate_count, seconds, CPU, success
    (10, 1800, 15, 1800, 275, True),
    (10, 1800, 16, 1800, 250, False),
    (10, 1800, 15, 1800, 275.01, False),
    (0, 1800, 0, 1800, 250, True),
    (0, 1800, 1, 1800, 250, False),
    (10, 1800, 16, 1920, 250, True),
]
for count, seconds, candidate, elapsed, cpu, expected in cases:
    script = f'''set -eu
P4D_BASELINE_OVERRUNS={count}; P4D_BASELINE_SECONDS={seconds}
P4D_BASELINE_CPU=250; overrun_delta={candidate}; elapsed={elapsed}; cpu={cpu}
''' + guards
    result = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
    assert (result.returncode == 0) == expected, (script, result.stderr)
metrics = (ROOT/'toolchain/p4d-metrics.sh').read_text()
finish = metrics.split('metrics_finish() {\n', 1)[1].split('\n}', 1)[0]
for elapsed, delta, expected in [(1800, 12, True), (1799, 12, False), (1800, -1, False)]:
    script = f'''set -eu
epoch={elapsed}; first_epoch=0; overruns={max(delta,0)}; first_overruns={max(-delta,0)}
process=450000; first_process=0; total=720000; first_total=0; cores=4
''' + 'metrics_finish() {\n' + finish + '\n}\nmetrics_finish\n'
    result = subprocess.run(['sh', '-c', script], capture_output=True, text=True)
    assert (result.returncode == 0) == expected, result.stderr
    if expected:
        assert 'cpu=250.000000' in result.stdout, result.stdout
print('P4d real metrics: 6 rate/CPU limits + duration/counter/4-core normalization PASS')
