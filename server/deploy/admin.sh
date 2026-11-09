#!/usr/bin/env bash
# Root-owned helper; environment contains paths/settings, never printed credentials.
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'sudo required' >&2; exit 2; }
ulimit -c 0
# Key entry needs root only for the protected atomic file replacement. It does
# not load the database/data key or inherit credentials from environment files.
if [[ ${1:-} == set-ark-key ]]; then
  [[ $# == 1 ]] || { echo 'set-ark-key accepts no arguments' >&2; exit 2; }
  cd /opt/xiaoai-memory
  exec /opt/xiaoai-memory/.venv/bin/python -m server.manage set-ark-key
fi
set -a
source /etc/xiaoai-memory.env
set +a
cd /opt/xiaoai-memory
exec runuser -u xiaoai -- /opt/xiaoai-memory/.venv/bin/python -m server.manage "$@"
