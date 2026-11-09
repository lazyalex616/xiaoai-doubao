#!/usr/bin/env bash
# Restore into a disposable private directory; never replace the live database.
set -euo pipefail
[[ $(id -u) == 0 ]] || exit 2
systemctl start xiaoai-memory-backup.service
set -a
source /etc/xiaoai-memory.env
set +a
cd /opt/xiaoai-memory
runuser -u xiaoai -- /opt/xiaoai-memory/.venv/bin/python - <<'PY'
from pathlib import Path
import tempfile
from server.backup import restore
from server.config import data_key
folder=Path('/var/backups/xiaoai-memory')
latest=max(folder.glob('*.backup.enc'),key=lambda p:p.stat().st_mtime_ns)
with tempfile.TemporaryDirectory(prefix='restore-check-',dir=folder) as tmp:
    store=restore(latest,Path(tmp)/'restored.sqlite',data_key(),folder/'latest.deletions')
    with store.db() as db:
        assert db.execute('PRAGMA integrity_check').fetchone()[0]=='ok'
print('Encrypted backup restore: integrity PASS; test files removed.')
PY
