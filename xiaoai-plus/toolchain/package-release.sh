#!/usr/bin/env bash
set -euo pipefail
umask 077
root=$(cd "$(dirname "$0")/.." && pwd)
python3 - "$root" <<'PY'
import gzip, pathlib, tarfile, sys
r=pathlib.Path(sys.argv[1]); out=r.parent/'dist';out.mkdir(exist_ok=True)
p=out/'xiaoai-doubao-oh2p-v0.1.0.tar.gz'
# Model weights, tokens, pronunciation lexicon and CA bundle are downloaded by setup.
files=[r/'build-armv7/xiaoai_plus_speaker',r/'boot.sh',r/'config.ini.example',r/'LICENSE']
files+=sorted((r/'keywords').glob('*.txt'))
files+=sorted((r/'assets/licenses').rglob('*')) if (r/'assets/licenses').exists() else []
with p.open('wb') as f, gzip.GzipFile(filename='',mode='wb',fileobj=f,mtime=0) as z, tarfile.open(fileobj=z,mode='w') as t:
 for src in files:
  if not src.is_file():continue
  rel=src.relative_to(r).as_posix() if src.parent.name!='build-armv7' else src.name
  info=t.gettarinfo(str(src),'oh2p/'+rel);info.uid=info.gid=0;info.uname=info.gname='';info.mtime=0
  info.mode=0o755 if src.name in ('boot.sh','xiaoai_plus_speaker') else 0o644
  with src.open('rb') as data:t.addfile(info,data)
p.chmod(0o600); print(p)
PY
