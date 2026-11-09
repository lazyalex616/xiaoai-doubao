#!/usr/bin/env bash
# Run only on the dedicated server in an explicitly authorized deployment task.
set -euo pipefail
if [[ ${1:-} != --execute-on-authorized-server ]]; then
  echo 'Usage: sudo bash server/deploy/install.sh --execute-on-authorized-server'
  echo 'Creates application user, copies server source, installs Python deps and systemd unit; does not start service.'
  exit 2
fi
[[ $(id -u) == 0 ]] || { echo 'root required'; exit 2; }
source_dir=$(cd "$(dirname "$0")/../.." && pwd)
[[ ! -e /opt/xiaoai-memory/server ]] || { echo 'Existing install detected; make a versioned backup before updating.'; exit 2; }
getent passwd xiaoai >/dev/null || useradd --system --home /var/lib/xiaoai --shell /usr/sbin/nologin xiaoai
install -d -m 0755 /opt/xiaoai-memory
install -d -m 0700 -o xiaoai -g xiaoai /var/lib/xiaoai
# Copy only reviewed source/assets; never local data, TLS keys or a developer venv.
python3 - "$source_dir/server" <<'PY'
import shutil,sys
from pathlib import Path
src=Path(sys.argv[1]);dst=Path('/opt/xiaoai-memory/server');dst.mkdir()
for name in ['__init__.py','app.py','models.py','store.py','worker.py','learning.py','notifications.py','manage.py','ark_setup.py','ark_summary.py','ark_identity.py','backup.py','config.py','requirements.txt','README.md']:
    shutil.copyfile(src/name,dst/name)
shutil.copytree(src/'web',dst/'web',ignore=shutil.ignore_patterns('*.pem','*.key','.env*'))
PY
python3 -m venv /opt/xiaoai-memory/.venv
/opt/xiaoai-memory/.venv/bin/pip install -r /opt/xiaoai-memory/server/requirements.txt
install -m 0644 "$source_dir/server/deploy/xiaoai-memory.service" /etc/systemd/system/xiaoai-memory.service
install -m 0644 "$source_dir/server/deploy/xiaoai-memory-backup.service" /etc/systemd/system/xiaoai-memory-backup.service
install -m 0644 "$source_dir/server/deploy/xiaoai-memory-backup.timer" /etc/systemd/system/xiaoai-memory-backup.timer
systemctl daemon-reload
echo 'Installed but not started. Configure private environment, provision family/device, validate Caddy; then authorize start.'
