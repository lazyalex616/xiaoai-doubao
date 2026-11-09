#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
sandbox=$(mktemp -d /tmp/oh2p-preserve-config-mock.XXXXXX)
trap 'rm -rf "$sandbox"' EXIT

setup_case() {
  root=$1
  mkdir -m 0700 "$root" "$root/xiaoai-plus" "$root/.xiaoai-stage-pcm-20260926" \
    "$root/.xiaoai-stage-pcm-20260926/oh2p"
  mkdir -m 0700 "$root/.xiaoai-stage-pcm-20260926/oh2p/keywords" \
    "$root/.xiaoai-stage-pcm-20260926/oh2p/assets"
  printf 'old\n' > "$root/xiaoai-plus/xiaoai_plus_speaker"
  cat > "$root/.xiaoai-stage-pcm-20260926/oh2p/xiaoai_plus_speaker" <<'SH'
#!/bin/sh
if [ -e "${0%/*}/reject" ]; then exit 1; fi
[ "$1" = -c ] && [ "$3" = --check-config ]
SH
  chmod 0700 "$root/.xiaoai-stage-pcm-20260926/oh2p/xiaoai_plus_speaker"
  printf 'boot\n' > "$root/xiaoai-plus/boot.sh"
  printf 'boot\n' > "$root/.xiaoai-stage-pcm-20260926/oh2p/boot.sh"
  chmod 0600 "$root/xiaoai-plus/boot.sh" "$root/.xiaoai-stage-pcm-20260926/oh2p/boot.sh"
  printf 'ca\n' > "$root/.xiaoai-stage-pcm-20260926/oh2p/assets/ca-certificates.crt"
  printf 'word\n' > "$root/.xiaoai-stage-pcm-20260926/oh2p/keywords/keywords_doubao_canonical.txt"
  cat > "$root/xiaoai-plus/config.ini" <<'INI'
[realtime]
api_key = fake-local-test-only
[wakeup]
keywords_file = /data/xiaoai-plus/keywords/keywords_doubao_canonical.txt
INI
  chmod 0600 "$root/xiaoai-plus/config.ini"
}

root=$sandbox/pass
setup_case "$root"
old_hash=$(sha256sum "$root/xiaoai-plus/xiaoai_plus_speaker" | awk '{print $1}')
new_hash=$(sha256sum "$root/.xiaoai-stage-pcm-20260926/oh2p/xiaoai_plus_speaker" | awk '{print $1}')
sh "$repo/toolchain/preserve-config-update-remote.sh" --mock "$root" "$old_hash" "$new_hash" \
  | grep -qx OFFLINE_UPDATE_WITH_CONFIG_SWITCHED
[ -f "$root/xiaoai-plus/config.ini" ]
[ -f "$root/xiaoai-plus.rollback-before-pcm-20260926/config.ini" ]
[ ! -x "$root/xiaoai-plus/boot.sh" ]
[ "$(sha256sum "$root/xiaoai-plus/config.ini" | awk '{print $1}')" = \
  "$(sha256sum "$root/xiaoai-plus.rollback-before-pcm-20260926/config.ini" | awk '{print $1}')" ]

root=$sandbox/reject
setup_case "$root"
old_hash=$(sha256sum "$root/xiaoai-plus/xiaoai_plus_speaker" | awk '{print $1}')
new_hash=$(sha256sum "$root/.xiaoai-stage-pcm-20260926/oh2p/xiaoai_plus_speaker" | awk '{print $1}')
: > "$root/.xiaoai-stage-pcm-20260926/oh2p/reject"
if sh "$repo/toolchain/preserve-config-update-remote.sh" --mock "$root" "$old_hash" "$new_hash" >/dev/null 2>&1; then
  echo UNEXPECTED_MIGRATION_SUCCESS >&2
  exit 1
fi
[ -f "$root/xiaoai-plus/config.ini" ]
[ ! -e "$root/.xiaoai-stage-pcm-20260926/oh2p/config.ini" ]
[ ! -e "$root/xiaoai-plus.rollback-before-pcm-20260926" ]

rm "$root/.xiaoai-stage-pcm-20260926/oh2p/reject"
ln -s "$root/xiaoai-plus/config.ini" "$root/.xiaoai-stage-pcm-20260926/oh2p/config.ini"
if sh "$repo/toolchain/preserve-config-update-remote.sh" --mock "$root" "$old_hash" "$new_hash" >/dev/null 2>&1; then
  echo SYMLINK_MIGRATION_ACCEPTED >&2
  exit 1
fi
[ -L "$root/.xiaoai-stage-pcm-20260926/oh2p/config.ini" ]
[ ! -e "$root/xiaoai-plus.rollback-before-pcm-20260926" ]

echo PRESERVE_CONFIG_MOCK_PASS
