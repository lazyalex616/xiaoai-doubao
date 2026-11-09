#!/usr/bin/env bash
set -eu
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
  printf '%s\n' '检查Apple芯片Mac、Homebrew、libusb、libusb-compat、squashfs、xz、python3和ssh。' '缺什么就按打印的命令自行安装；本脚本不安装任何软件。' 'OK: help complete' 'NEXT: bash setup/01-check-mac.sh'
  exit 0
fi
if [[ $(uname -s) != Darwin || $(uname -m) != arm64 ]]; then
  printf '%s\n' 'STOP: 只支持Apple芯片Mac；Windows/Linux未验证' 'NEXT: 使用Apple芯片Mac，其他平台参阅上游说明'
  exit 1
fi
missing=()
brew_missing=0
if ! command -v brew >/dev/null; then
  brew_missing=1
  printf '%s\n' '先从 https://brew.sh 安装Homebrew'
fi
for formula in libusb libusb-compat squashfs xz; do
  prefix=''
  if [[ $brew_missing == 0 ]]; then prefix=$(brew --prefix "$formula" 2>/dev/null || true); fi
  if [[ -z $prefix || ! -d $prefix ]]; then missing+=("$formula"); fi
done
for cmd in python3 ssh; do
  if ! command -v "$cmd" >/dev/null; then
    if [[ $cmd == ssh ]]; then missing+=(openssh); else missing+=(python3); fi
  fi
done
if [[ ${#missing[@]} != 0 ]]; then printf 'brew install'; printf ' %s' "${missing[@]}"; printf '\n'; fi
if [[ $brew_missing != 0 || ${#missing[@]} != 0 ]]; then
  printf '%s\n' 'STOP: 缺少依赖' 'NEXT: 自行安装上述依赖，然后重跑 bash setup/01-check-mac.sh'
  exit 1
fi
printf '%s\n' 'OK: Mac依赖齐全' 'NEXT: bash setup/02-get-firmware.sh'
