#!/bin/sh
set -eu
umask 077

# 纯自启动脚本：开机时拉起 xiaoai_plus_speaker

APP_DIR="/data/xiaoai-plus"
BIN_PATH="/data/xiaoai-plus/xiaoai_plus_speaker"
CFG_PATH="/data/xiaoai-plus/config.ini"
LOG_PATH="/data/xiaoai-plus/xiaoai_plus.log"
WAIT_HOST="223.5.5.5"
WAIT_SECONDS=60

if [ ! -x "${BIN_PATH}" ]; then
  echo "错误：未找到可执行文件 ${BIN_PATH}" >&2
  exit 1
fi

if [ ! -f "${CFG_PATH}" ]; then
  echo "错误：未找到配置文件 ${CFG_PATH}" >&2
  exit 1
fi
if [ ! -f "${APP_DIR}/assets/ca-certificates.crt" ]; then
  echo "错误：缺少受信任 CA 证书包" >&2
  exit 1
fi
if [ -L "${APP_DIR}" ] || [ -L "${LOG_PATH}" ] || [ -L "${LOG_PATH}.1" ]; then
  echo "错误：程序目录或日志不能是符号链接" >&2
  exit 1
fi
cd "${APP_DIR}"
if ! "${BIN_PATH}" --check-config -c "${CFG_PATH}" >/dev/null; then
  echo "错误：配置权限或格式未通过检查" >&2
  exit 1
fi
chmod 0700 "${APP_DIR}"
touch "${LOG_PATH}"
chmod 0600 "${LOG_PATH}"
# Keep stdout's open descriptor valid and retain the previous 4 MiB.
# Together the current file and one archive use at most about 8 MiB.
rotate_log() {
  if [ "$(wc -c < "${LOG_PATH}")" -ge 4194304 ]; then
    tail -c 4194304 "${LOG_PATH}" > "${LOG_PATH}.1"
    chmod 0600 "${LOG_PATH}.1"
    : > "${LOG_PATH}"
    echo "guardian: log rotated $(date '+%Y-%m-%d %H:%M:%S')" >> "${LOG_PATH}"
  fi
}
rotate_log

if command -v ping >/dev/null 2>&1; then
  i=0
  while [ "${i}" -lt "${WAIT_SECONDS}" ]; do
    if ping -c 1 "${WAIT_HOST}" >/dev/null 2>&1; then
      echo "网络已就绪"
      break
    fi
    if [ "${i}" -eq 0 ]; then
      echo "等待网络就绪中..."
    fi
    i=$((i + 1))
    sleep 1
  done
fi

# Keep one local guardian. The startup hook may invoke this script with /bin/sh
# while it remains mode 0600 until the separately approved autostart phase.
PID_FILE="${APP_DIR}/guardian.pid"
if [ -L "${PID_FILE}" ]; then
  echo "错误：守护 PID 文件不能是符号链接" >&2
  exit 1
fi
if [ -f "${PID_FILE}" ]; then
  old_pid="$(cat "${PID_FILE}")"
  case "${old_pid}" in
    ''|*[!0-9]*) echo "错误：守护 PID 文件无效" >&2; exit 1 ;;
  esac
  if kill -0 "${old_pid}" 2>/dev/null &&
    tr '\0' ' ' < "/proc/${old_pid}/cmdline" 2>/dev/null | grep -q 'xiaoai-plus/boot.sh'; then
    echo "守护进程已运行：${old_pid}"
    exit 0
  fi
fi

(
  umask 077
  failure_times=''
  delay=1
  while :; do
    rotate_log
    started="$(date +%s)"
    echo "guardian: starting speaker" >> "${LOG_PATH}"
    "${BIN_PATH}" -c "${CFG_PATH}" >> "${LOG_PATH}" 2>&1 &
    child=$!
    # The guardian waits for the actual child. A separate monitor bounds the
    # log while it is alive; no status-file polling can outlive a dead child.
    (
      while :; do
        sleep 1
        rotate_log
      done
    ) &
    monitor=$!
    rc=0
    wait "${child}" || rc=$?
    kill "${monitor}" 2>/dev/null || true
    wait "${monitor}" 2>/dev/null || true
    ended="$(date +%s)"
    recent=''
    for failed_at in ${failure_times}; do
      if [ "$((ended - failed_at))" -lt 1800 ]; then
        recent="${recent} ${failed_at}"
      fi
    done
    failure_times="${recent} ${ended}"
    set -- ${failure_times}
    failures=$#
    if [ "$((ended - started))" -ge 300 ]; then
      delay=1
    fi
    echo "guardian: speaker exit rc=${rc} failures_30m=${failures}" >> "${LOG_PATH}"
    if [ "${failures}" -ge 5 ]; then
      echo "guardian: stopped after five failures in 30 minutes" >> "${LOG_PATH}"
      break
    fi
    sleep "${delay}"
    delay=$((delay * 2))
  done
  rm -f "${PID_FILE}"
) </dev/null >/dev/null 2>&1 &
echo "$!" > "${PID_FILE}"
chmod 0600 "${PID_FILE}"
echo "守护进程已启动，日志文件：${LOG_PATH}"
