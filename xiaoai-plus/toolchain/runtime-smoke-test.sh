#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BINARY_PATH="${PROJECT_ROOT}/build-armv7/xiaoai_plus_speaker"
IMAGE_NAME="xiaoai-plus-runtime-test:dev"
RUN_SECONDS=8

if [[ ! -x "${BINARY_PATH}" ]]; then
  echo "错误：未找到可执行文件 ${BINARY_PATH}" >&2
  echo "请先执行：make -C toolchain compile" >&2
  exit 1
fi

echo "[runtime-smoke] build runtime image: ${IMAGE_NAME}"
echo "[runtime-smoke] ensure base toolchain image: xiaoai-plus-toolchain:dev"
docker build --platform linux/amd64 \
  -t xiaoai-plus-toolchain:dev \
  "${PROJECT_ROOT}/toolchain"

docker build --platform linux/amd64 \
  -t "${IMAGE_NAME}" \
  -f "${PROJECT_ROOT}/toolchain/runtime-test.Dockerfile" \
  "${PROJECT_ROOT}/toolchain"

echo "[runtime-smoke] start app in container for ${RUN_SECONDS}s"
docker run --rm --platform linux/amd64 --network none \
  -v "${PROJECT_ROOT}:/workspace" \
  -w /workspace \
  "${IMAGE_NAME}" \
  bash -lc '
set -euo pipefail
qemu-arm-static ./build-armv7/xiaoai_plus_speaker --kws-only --keywords-file assets/keywords.txt &
pid=$!
sleep '"${RUN_SECONDS}"'
if ! kill -0 "$pid" 2>/dev/null; then
  wait "$pid"
fi
kill -TERM "$pid"
wait "$pid"
'

echo "[runtime-smoke] success"
