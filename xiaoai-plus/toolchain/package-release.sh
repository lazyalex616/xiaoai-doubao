#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/dist/release"
mkdir -p "$out/oh2p/assets" "$out/oh2p/keywords"
install -m 0755 "$root/build-armv7/xiaoai_plus_speaker" "$root/boot.sh" "$out/oh2p/"
install -m 0644 "$root/config.ini.example" "$out/oh2p/"
for name in encoder.onnx decoder.onnx joiner.onnx tokens.txt keywords.txt ca-certificates.crt; do
  install -m 0644 "$root/assets/$name" "$out/oh2p/assets/"
done
install -m 0644 "$root/keywords/"*.txt "$out/oh2p/keywords/"
tar -C "$out" -czf "$out/oh2p.tar.gz" oh2p
printf 'Package: %s\n' "$out/oh2p.tar.gz"
